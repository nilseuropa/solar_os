/* SolarOS ESP-IDF 5.5.5 overlay. Included inside ble_gatts.c's dynamic gate.
 * Runtime registration is a transaction under the host lock. No global startup
 * definition array is borrowed, and callbacks are published only after commit.
 */
void ble_att_svr_rollback(uint16_t checkpoint);

struct solar_os_gatts_registration {
    struct solar_os_gatts_registration *next;
    struct ble_gatt_register_ctxt ctxt;
};

struct solar_os_gatts_transaction {
    uint16_t checkpoint;
    uint16_t handle, allowed;
    int error;
    struct solar_os_gatts_registration *first, **last;
};

static void
solar_os_gatts_record(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    struct solar_os_gatts_transaction *tx = arg;
    struct solar_os_gatts_registration *record;
    if (tx->error) {
        return;
    }
    record = nimble_platform_mem_calloc(1, sizeof *record);
    if (!record) {
        tx->error = BLE_HS_ENOMEM;
        return;
    }
    record->ctxt = *ctxt;
    *tx->last = record;
    tx->last = &record->next;
}

static int
solar_os_gatts_add_cfg(struct ble_gatts_clt_cfg_list *list,
                      uint16_t handle, uint16_t allowed)
{
    struct ble_gatts_clt_cfg *cfg = ble_gatts_clt_cfg_alloc();
    if (!cfg) {
        return BLE_HS_ENOMEM;
    }
    cfg->chr_val_handle = handle;
    cfg->allowed = allowed;
    cfg->flags = 0;
    STAILQ_INSERT_TAIL(list, cfg, next);
    return 0;
}

static int
solar_os_gatts_add_conn_cfg(struct ble_hs_conn *conn, void *arg)
{
    struct solar_os_gatts_transaction *tx = arg;
    tx->error = solar_os_gatts_add_cfg(&conn->bhc_gatt_svr.clt_cfgs,
                                      tx->handle, tx->allowed);
    if (!tx->error) {
        conn->bhc_gatt_svr.num_clt_cfgs++;
    }
    return tx->error;
}

static int
solar_os_gatts_trim_cfg(struct ble_gatts_clt_cfg_list *list, uint16_t checkpoint)
{
    struct ble_gatts_clt_cfg *cfg, *next;
    int removed = 0;
    for (cfg = STAILQ_FIRST(list); cfg; cfg = next) {
        next = STAILQ_NEXT(cfg, next);
        if (cfg->chr_val_handle > checkpoint) {
            STAILQ_REMOVE(list, cfg, ble_gatts_clt_cfg, next);
            ble_gatts_clt_cfg_free(cfg);
            removed++;
        }
    }
    return removed;
}

static int
solar_os_gatts_trim_conn_cfg(struct ble_hs_conn *conn, void *arg)
{
    struct solar_os_gatts_transaction *tx = arg;
    conn->bhc_gatt_svr.num_clt_cfgs -=
        solar_os_gatts_trim_cfg(&conn->bhc_gatt_svr.clt_cfgs, tx->checkpoint);
    return 0;
}

int
ble_gatts_add_dynamic_svcs(const struct ble_gatt_svc_def *svcs)
{
    struct solar_os_gatts_transaction tx = {0};
    struct solar_os_gatts_registration *record;
    struct ble_gatts_svc_entry *entry, *tail = NULL, *next;
    struct ble_att_svr_entry *ha;
    const struct ble_gatt_chr_def *chr;
    ble_uuid16_t uuid = BLE_UUID16_INIT(BLE_ATT_UUID_CHARACTERISTIC);
    int old_count, rc, i;
    uint16_t end_handle;

    if (!svcs || svcs[0].type == BLE_GATT_SVC_TYPE_END) {
        return BLE_HS_EINVAL;
    }
    ble_hs_lock();
#if MYNEWT_VAL(BLE_STATIC_TO_DYNAMIC)
    if (ble_gatts_ensure_ctx()) {
        ble_hs_unlock();
        return BLE_HS_ENOMEM;
    }
#endif
    /* Runtime registration must not steal pending startup definitions or
     * register the same service definition twice. */
    if (ble_gatts_svc_defs != NULL) {
        ble_hs_unlock();
        return BLE_HS_EBUSY;
    }
    for (i = 0; svcs[i].type != BLE_GATT_SVC_TYPE_END; i++) {
        if (ble_gatts_find_svc_entry(&svcs[i])) {
            ble_hs_unlock();
            return BLE_HS_EALREADY;
        }
    }
    STAILQ_FOREACH(entry, &ble_gatts_svc_entries, next) {
        if (!entry->handle) {
            ble_hs_unlock();
            return BLE_HS_EBUSY;
        }
        tail = entry;
    }
    tx.checkpoint = ble_att_svr_prev_handle();
    tx.last = &tx.first;
    old_count = ble_gatts_num_cfgable_chrs;
    rc = ble_gatts_register_svcs(svcs,
        ble_hs_cfg.gatts_register_cb ? solar_os_gatts_record : NULL, &tx);
    if (!rc) {
        rc = tx.error;
    }
    if (rc) {
        goto rollback;
    }

    /* Start at the previous high-water mark, not the last CCCD. The latter
     * re-adds existing configurations when old services have been deleted. */
    ha = NULL;
    while ((ha = ble_att_svr_find_by_uuid(ha, &uuid.u, UINT16_MAX)) != NULL) {
        if (ha->ha_handle_id <= tx.checkpoint) {
            continue;
        }
        chr = ha->ha_cb_arg;
        tx.allowed = ble_gatts_chr_clt_cfg_allowed(chr);
        if (!tx.allowed) {
            continue;
        }
        tx.handle = ha->ha_handle_id + 1;
        rc = solar_os_gatts_add_cfg(&ble_gatts_clt_cfgs, tx.handle, tx.allowed);
        if (rc) {
            goto rollback;
        }
        ble_hs_conn_foreach(solar_os_gatts_add_conn_cfg, &tx);
        if (tx.error) {
            rc = tx.error;
            goto rollback;
        }
    }
    end_handle = ble_att_svr_prev_handle();
    while ((record = tx.first) != NULL) {
        tx.first = record->next;
        ble_hs_cfg.gatts_register_cb(&record->ctxt, ble_hs_cfg.gatts_register_arg);
        nimble_platform_mem_free(record);
    }
#if MYNEWT_VAL(BLE_GATT_CACHING)
#if MYNEWT_VAL(BLE_STATIC_TO_DYNAMIC)
    if (ble_gatts_conn_aware_states != NULL) {
#endif
    for (i = 0; i < MYNEWT_VAL(BLE_STORE_MAX_BONDS); i++) {
        ble_gatts_conn_aware_states[i].half_aware = 0;
        ble_gatts_conn_aware_states[i].aware = false;
    }
    ble_hs_conn_foreach(ble_gatts_conn_unaware, NULL);
#if MYNEWT_VAL(BLE_STATIC_TO_DYNAMIC)
    }
#endif
#endif
    ble_svc_gatt_changed(tx.checkpoint + 1, end_handle);
    ble_hs_unlock();
    return 0;

rollback:
    ble_hs_conn_foreach(solar_os_gatts_trim_conn_cfg, &tx);
    solar_os_gatts_trim_cfg(&ble_gatts_clt_cfgs, tx.checkpoint);
    ble_att_svr_rollback(tx.checkpoint);
    entry = tail ? STAILQ_NEXT(tail, next) : STAILQ_FIRST(&ble_gatts_svc_entries);
    while (entry) {
        next = STAILQ_NEXT(entry, next);
        STAILQ_REMOVE(&ble_gatts_svc_entries, entry, ble_gatts_svc_entry, next);
        ble_gatts_svc_entry_free(entry);
        entry = next;
    }
    ble_gatts_num_cfgable_chrs = old_count;
    for (i = 0; svcs[i].type != BLE_GATT_SVC_TYPE_END; i++) {
        if (svcs[i].characteristics) {
            for (chr = svcs[i].characteristics; chr->uuid; chr++) {
                if (chr->val_handle) {
                    *chr->val_handle = 0;
                }
            }
        }
    }
    while ((record = tx.first) != NULL) {
        tx.first = record->next;
        nimble_platform_mem_free(record);
    }
    ble_hs_unlock();
    return rc;
}
