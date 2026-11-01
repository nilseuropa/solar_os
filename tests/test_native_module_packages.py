from pathlib import Path
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[1]


class NativeModulePackagesTest(unittest.TestCase):
    def test_hello_is_an_official_versioned_module(self):
        module = ROOT / "modules" / "examples" / "hello"
        with (module / "module.toml").open("rb") as stream:
            manifest = tomllib.load(stream)

        self.assertEqual(manifest["schema_version"], 2)
        self.assertEqual(manifest["type"], "app")
        self.assertEqual(manifest["id"], "hello")
        self.assertEqual(manifest["target"], "esp32s3")
        self.assertEqual(manifest["native_abi"], 1)
        self.assertEqual(manifest["lifecycle_abi"], 1)
        self.assertEqual(manifest["minimum_host_api_size"], 20)
        self.assertEqual(manifest["required_capabilities"], ["psram"])
        self.assertTrue((module / "main" / "hello.c").is_file())

    def test_resident_acceptance_modules_have_typed_abis(self):
        expected = {
            "hello-job": ("job", "solar_os_native_job_host_v1"),
            "hello-driver": ("driver", "solar_os_native_driver_host_v1"),
        }
        for module_id, (module_type, symbol) in expected.items():
            module = ROOT / "modules" / "examples" / module_id
            with (module / "module.toml").open("rb") as stream:
                manifest = tomllib.load(stream)
            self.assertEqual(manifest["type"], module_type)
            self.assertEqual(manifest["id"], module_id)
            self.assertEqual(manifest["lifecycle_abi"], 1)
            self.assertTrue(manifest["artifact"].endswith(f".{module_type}.elf"))
            source = next((module / "main").glob("*.c")).read_text(encoding="utf-8")
            self.assertIn(symbol, source)

    def test_job_host_exposes_versioned_ble_service_without_nimble_types(self):
        job_header = (ROOT / "include" / "solar_os_native_job_abi.h").read_text(
            encoding="utf-8"
        )
        ble_header = (ROOT / "include" / "solar_os_native_ble_abi.h").read_text(
            encoding="utf-8"
        )
        native = (ROOT / "src" / "services" / "solar_os_native.c").read_text(
            encoding="utf-8"
        )

        self.assertGreater(job_header.index("get_service"), job_header.index("register_job"))
        self.assertIn('SOLAR_OS_NATIVE_BLE_CLIENT_SERVICE "ble.client"', ble_header)
        self.assertIn("SOLAR_OS_NATIVE_BLE_VALUE_MAX 176U", ble_header)
        self.assertNotIn("nimble", ble_header.lower())
        self.assertNotIn("esp_err", ble_header)
        for operation in (
            "session_create",
            "session_cancel",
            "session_close",
            "peer_connect",
            "peer_disconnect",
            "peer_pair",
            "peer_services",
            "peer_characteristics",
            "peer_configure_queue",
            "peer_subscribe",
            "peer_poll",
            "peer_read",
            "peer_write",
        ):
            self.assertIn(f"(*{operation})", ble_header)
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_BLE", native)
        self.assertIn(".get_service = native_job_get_service", native)
        self.assertIn("SOLAR_OS_MEMORY_TRANSIENT", native)

    def test_resident_slots_are_allocated_lazily_in_external_memory(self):
        native = (ROOT / "src/services/solar_os_native.c").read_text(
            encoding="utf-8"
        )

        self.assertNotIn(
            "native_resident_slot_t native_resident_slots[", native
        )
        allocation_start = native.index("slot = solar_os_memory_calloc(")
        allocation_end = native.index("slot->type = type;", allocation_start)
        allocation = native[allocation_start:allocation_end]
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_REQUIRED", allocation)
        self.assertIn('"native.resident.slot"', allocation)

        deactivation_start = native.index(
            "esp_err_t solar_os_native_module_deactivate("
        )
        deactivation = native[deactivation_start:]
        self.assertIn("native_slot_remove(slot);", deactivation)
        self.assertIn("solar_os_memory_free(slot);", deactivation)

    def test_native_package_service_has_compatibility_and_integrity_gates(self):
        service = (ROOT / "src/services/solar_os_module_packages.c").read_text(
            encoding="utf-8"
        )
        for contract in (
            "SOLAR_OS_OTA_PUBLIC_KEY_PEM",
            "SOLAR_OS_VERSION",
            "CONFIG_IDF_TARGET",
            "SOLAR_OS_NATIVE_ABI_VERSION",
            "minimum_host_api_size",
            "required_capabilities",
            "solar_os_download_file",
            "package->artifact_size, package->artifact_sha256",
            "solar_os_native_elf_validate",
            "solar_os_storage_replace_file",
            "SOLAR_OS_MODULE_TYPE_APP",
            "SOLAR_OS_MODULE_TYPE_JOB",
            "SOLAR_OS_MODULE_TYPE_DRIVER",
            "MODULE_CATALOG_SCHEMA_VERSION",
            "SOLAR_OS_MODULE_APP_LIFECYCLE_ABI",
            "SOLAR_OS_MODULE_JOB_LIFECYCLE_ABI",
            "SOLAR_OS_MODULE_DRIVER_LIFECYCLE_ABI",
            "solar_os_native_module_activate",
            "solar_os_native_module_deactivate",
            '"lifecycle_abi"',
            '"modules/%s/%s.elf"',
            '"%s/%s/%s/%s.elf"',
            '"modules/%s.app.elf"',
        ):
            self.assertIn(contract, service)

    def test_pkg_and_package_catalog_are_wired_together(self):
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/shell/solar_os_shell_system.c").read_text(
            encoding="utf-8"
        )

        self.assertIn('"services/solar_os_module_packages.c"', packages)
        for dependency in (
            '"service_crypto"',
            '"service_http_client"',
            '"service_json"',
        ):
            self.assertIn(dependency, packages)
        for command in ("available", "installed", "install", "remove"):
            self.assertIn(f'strcmp(argv[1], "{command}")', shell)

        self.assertIn("PKG_NETWORK_TASK_STACK (16U * 1024U)", shell)
        self.assertIn("solar_os_task_create_pinned_internal(pkg_network_task", shell)
        self.assertIn("pkg_run_network_worker(term, PKG_NETWORK_AVAILABLE", shell)
        self.assertIn("pkg_run_network_worker(term, PKG_NETWORK_INSTALL", shell)
        self.assertIn("ch == SOLAR_OS_KEY_ESCAPE", shell)
        self.assertIn("ch == 0x03U", shell)

    def test_pkg_has_tui_lists_and_module_completion(self):
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        shell_command = (ROOT / "src/shell/solar_os_shell_system.c").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
        tui = (ROOT / "src/shell/solar_os_shell_pkg_tui.c").read_text(
            encoding="utf-8"
        )

        self.assertIn('"shell/solar_os_shell_pkg_tui.c"', packages)
        self.assertIn("solar_os_shell_launch_pkg_tui(ctx)", shell_command)
        self.assertIn("pkg_print_installed(term)", shell_command)
        self.assertIn("solar_os_module_package_foreach_installed", shell_command)
        self.assertIn("SHELL_COMPLETION_MODULES_AVAILABLE(path_pkg_install)", shell)
        self.assertIn("SHELL_COMPLETION_MODULES_INSTALLED(path_pkg_remove)", shell)
        self.assertIn("solar_os_module_catalog_cached_get", shell)
        self.assertIn("solar_os_module_package_foreach_installed", shell)
        self.assertIn("solar_os_module_catalog_fetch_ex", tui)
        self.assertIn("solar_os_module_package_install", tui)
        self.assertIn("solar_os_module_package_remove", tui)
        self.assertIn("SOLAR_OS_EVENT_TICK", tui)

    def test_job_completion_uses_runtime_registry(self):
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
        start = shell.index("static void shell_completion_emit_jobs(")
        end = shell.index("static void shell_completion_emit_schedule_entries(", start)
        completion = shell[start:end]

        self.assertIn("solar_os_jobs_count()", completion)
        self.assertIn("solar_os_jobs_get(i, &job)", completion)
        self.assertNotIn("solar_os_job_registry_count()", completion)

    def test_installed_application_modules_are_shell_commands(self):
        service = (ROOT / "src/services/solar_os_module_packages.c").read_text(
            encoding="utf-8"
        )
        native_shell = (ROOT / "src/shell/solar_os_shell_native.c").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")

        self.assertIn('static const char suffix[] = ".elf";', service)
        self.assertIn("solar_os_module_package_foreach_installed", service)
        self.assertIn('return "apps";', service)
        self.assertIn('return "jobs";', service)
        self.assertIn('return "drivers";', service)
        self.assertIn("solar_os_shell_try_native_module", native_shell)
        self.assertIn("SOLAR_OS_MODULE_TYPE_APP", native_shell)
        self.assertIn("native_shell_run(solar_os_shell_command_io(ctx)", native_shell)
        self.assertIn("solar_os_module_package_foreach_installed", shell)

        dispatch = shell[shell.index("static bool shell_execute_line(") :]
        alias = dispatch.index("if (alias_matched)")
        native = dispatch.index("solar_os_shell_try_native_module")
        unknown = dispatch.index("shell_report_unknown_command(io")
        self.assertLess(alias, native)
        self.assertLess(native, unknown)


if __name__ == "__main__":
    unittest.main()
