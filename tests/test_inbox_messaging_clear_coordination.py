import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
INBOX = (REPOSITORY / "src/services/solar_os_inbox.c").read_text(encoding="utf-8")
MESSAGING = (
    REPOSITORY / "src/services/solar_os_messaging.c"
).read_text(encoding="utf-8")


class InboxMessagingClearCoordinationTest(unittest.TestCase):
    def test_inbox_clear_notifies_after_releasing_inbox_lock(self):
        start = INBOX.index("esp_err_t solar_os_inbox_clear(void)")
        end = INBOX.index("const char *solar_os_inbox_priority_name", start)
        body = INBOX[start:end]
        self.assertLess(body.index("inbox_unlock();"), body.index("observer(observer_user);"))

    def test_messaging_unlinks_and_persists_projection_links(self):
        self.assertIn(
            "solar_os_inbox_set_clear_observer(messaging_unlink_all_inbox_projections",
            MESSAGING,
        )
        self.assertIn("slot->message.inbox_id = 0;", MESSAGING)
        self.assertIn("messaging_store_update(keys[i])", MESSAGING)

    def test_reused_inbox_ids_are_verified_before_mutation(self):
        self.assertGreaterEqual(
            MESSAGING.count("solar_os_inbox_matches_source_id("),
            3,
        )
        self.assertIn("messaging_reconcile_inbox_projections();", MESSAGING)


if __name__ == "__main__":
    unittest.main()
