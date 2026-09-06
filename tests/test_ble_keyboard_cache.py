from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/services/solar_os_ble_keyboard.c"


def function_body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class FakeForgetBoundary:
    """Small boundary model for the completion contract tested in the C code."""

    def __init__(self, peers):
        self.peers = list(peers)

    def forget(self, outcomes):
        remaining = list(self.peers)
        for peer in list(remaining):
            cache_result, api_result, event_bda, event_status = outcomes[peer]
            if cache_result != "ESP_OK":
                return remaining
            if api_result != "ESP_OK":
                return remaining
            if event_bda != peer:
                return remaining
            if event_status != "ESP_BT_STATUS_SUCCESS":
                return remaining
            remaining.remove(peer)
        return remaining


class BleKeyboardForgetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")

    def test_cache_clean_precedes_bond_request_and_completion_wait(self):
        remove_one = function_body(
            self.source, "static esp_err_t remove_one_deferred_bond(const uint8_t *bda"
        )
        self.assertLess(
            remove_one.index("esp_ble_gattc_cache_clean"),
            remove_one.index("begin_bond_remove_wait"),
        )
        self.assertLess(
            remove_one.index("esp_ble_remove_bond_device"),
            remove_one.index("wait_for_bond_remove_completion"),
        )

    def test_gap_completion_is_matched_before_signalling(self):
        gap = function_body(
            self.source, "static void gap_callback(esp_gap_ble_cb_event_t event"
        )
        self.assertIn("ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT", gap)
        self.assertIn("bond_remove_pending", gap)
        self.assertIn("memcmp(bda, bond_remove_bda", gap)
        self.assertIn("xSemaphoreGive(bond_remove_sem)", gap)

    def test_close_event_does_not_remove_bonds_before_forget_task(self):
        hidh = function_body(
            self.source,
            "static void hidh_callback(void *handler_args, esp_event_base_t base",
        )
        close = hidh[hidh.index("case ESP_HIDH_CLOSE_EVENT:") :]
        self.assertNotIn("remove_deferred_bonds();", close)

    def test_forget_does_not_clear_remembered_state_before_completion(self):
        forget = function_body(
            self.source, "static esp_err_t forget_remembered_keyboard(void)"
        )
        self.assertNotIn("clear_remembered_peers", forget)
        self.assertLess(
            forget.index("complete_deferred_bond_forget"),
            forget.index('"forgot keyboard"'),
        )
        self.assertIn("reconnect_suppressed_for_forget = true", forget)
        self.assertIn("reconnect_suppressed_for_forget = false", forget)

    def test_immediate_remove_failure_retains_peer(self):
        model = FakeForgetBoundary(["A"])
        self.assertEqual(
            model.forget({"A": ("ESP_OK", "ESP_FAIL", "A", "ESP_BT_STATUS_SUCCESS")}),
            ["A"],
        )

    def test_async_completion_failure_retains_peer(self):
        model = FakeForgetBoundary(["A"])
        self.assertEqual(
            model.forget({"A": ("ESP_OK", "ESP_OK", "A", "ESP_BT_STATUS_FAIL")}),
            ["A"],
        )

    def test_completion_timeout_retains_peer(self):
        model = FakeForgetBoundary(["A"])
        self.assertEqual(
            model.forget({"A": ("ESP_OK", "ESP_OK", None, "ESP_BT_STATUS_SUCCESS")}),
            ["A"],
        )

    def test_successful_completion_removes_peer(self):
        model = FakeForgetBoundary(["A"])
        self.assertEqual(
            model.forget({"A": ("ESP_OK", "ESP_OK", "A", "ESP_BT_STATUS_SUCCESS")}),
            [],
        )

    def test_unrelated_completion_does_not_satisfy_pending_operation(self):
        model = FakeForgetBoundary(["A"])
        self.assertEqual(
            model.forget({"A": ("ESP_OK", "ESP_OK", "B", "ESP_BT_STATUS_SUCCESS")}),
            ["A"],
        )

    def test_successful_a_and_failed_b_leave_only_b(self):
        model = FakeForgetBoundary(["A", "B"])
        self.assertEqual(
            model.forget(
                {
                    "A": ("ESP_OK", "ESP_OK", "A", "ESP_BT_STATUS_SUCCESS"),
                    "B": ("ESP_OK", "ESP_FAIL", "B", "ESP_BT_STATUS_SUCCESS"),
                }
            ),
            ["B"],
        )


if __name__ == "__main__":
    unittest.main()
