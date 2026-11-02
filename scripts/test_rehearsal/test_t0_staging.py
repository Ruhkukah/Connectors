"""Staging creates review copies; it neither edits originals nor grants execution."""
import copy
import json
import unittest

import t0_staging as stage


class T0StagingTests(unittest.TestCase):
    def sources(self):
        return {
            "runtime.py": self.runtime(),
            "helper.py": "T1_LINKS_SHA = '" + stage.T1_PROFILE + "'\n"
                         "ROUTE = 'links_public.t1.ini'\nCLIENT = 'client_t1.ini'\n",
        }

    def runtime(self):
        return ("SOURCE = '" + stage.SOURCE + "'\nREVIEW_ONLY = False\n"
                "AUTHORIZED_DATE = '2026-10-19'\nSTATE = '/private/t1/state.bin'\n"
                "REGISTRY = '/private/t1/registry.json'\n")

    def paths(self):
        return {"/private/t1/state.bin": "/private/t0/proposed-state.bin",
                "/private/t1/registry.json": "/private/t0/proposed-registry.json"}

    def test_copies_locked_and_routed_to_t0_originals_untouched_and_audit_sanitized(self):
        sources, paths = self.sources(), self.paths()
        originals, original_paths = copy.deepcopy(sources), copy.deepcopy(paths)
        copies, report = stage.stage_sources(sources, paths)
        self.assertEqual(sources, originals)
        self.assertEqual(paths, original_paths)
        self.assertIn("REVIEW_ONLY = True", copies["runtime.py"])
        self.assertIn("AUTHORIZED_DATE = '2026-10-21'", copies["runtime.py"])
        self.assertIn(stage.T0_PROFILE, copies["helper.py"])
        self.assertIn("links_public.t0.ini", copies["helper.py"])
        self.assertIn("client_t0.ini", copies["helper.py"])
        for text in copies.values():
            self.assertNotIn(stage.T1_PROFILE, text)
            self.assertNotIn("links_public.t1.ini", text)
        self.assertIn("/private/t0/proposed-state.bin", copies["runtime.py"])
        self.assertIs(report["can_arm"], False)
        self.assertIs(report["can_trade"], False)
        self.assertNotIn("/private", json.dumps(report))
        self.assertEqual([row["replacements"] for row in report["path_changes"]], [1, 1])
        for row in report["files"]:
            self.assertNotEqual(row["original_sha256"], row["staged_sha256"])

    def test_wrong_profile_or_candidate_pin_refused(self):
        for old in (stage.T1_PROFILE, stage.SOURCE):
            sources = {name: text.replace(old, "0" * len(old)) for name, text in self.sources().items()}
            with self.subTest(old=old), self.assertRaises(ValueError):
                stage.stage_sources(sources, self.paths())

    def test_second_wrong_profile_assignment_refused_even_if_good_pin_present(self):
        sources = self.sources()
        sources["wrong.py"] = "T1_LINKS_SHA = '" + "0" * 64 + "'\n"
        with self.assertRaisesRegex(ValueError, "wrong_profile_pin"):
            stage.stage_sources(sources, self.paths())

    def test_ambiguous_noop_absent_and_nonabsolute_path_map_refused(self):
        for paths in ({"/private/t1/state.bin": "/private/t1/state.bin"},
                      {"/missing": "/new"}, {"relative": "/new"},
                      {"/private/t1": "/new", "/private/t1/state.bin": "/different"},
                      {"/private/t1/state.bin": "/private/t1/registry.json"},
                      {"/private/t1/state.bin": "/new", "/private/t1/registry.json": "/new"}):
            with self.subTest(paths=paths), self.assertRaises(ValueError):
                stage.stage_sources(self.sources(), paths)

    def test_only_python_basename_sources_accepted(self):
        for name in ("../runtime.py", "/runtime.py", "binary.so", "manifest.json"):
            sources = self.sources()
            sources[name] = "x=1\n"
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "python_sources_only"):
                stage.stage_sources(sources, {})

    def test_review_lock_missing_dynamic_or_inline_assignment_is_refused(self):
        for replacement in ("", "REVIEW_ONLY = bool(0)", "x=0; REVIEW_ONLY = False", "REVIEW_ONLY: bool = False"):
            sources = self.sources()
            sources["runtime.py"] = sources["runtime.py"].replace("REVIEW_ONLY = False", replacement)
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                stage.stage_sources(sources, {})

    def test_every_existing_review_lock_stays_true(self):
        sources = self.sources()
        sources["extra.py"] = "REVIEW_ONLY = True\n"
        copies, _ = stage.stage_sources(sources, {})
        self.assertEqual(copies["extra.py"], "REVIEW_ONLY = True\n")

    def test_both_frozen_original_calendar_dates_become_review_only_wednesday(self):
        sources = self.sources()
        sources["short_runtime.py"] = "AUTHORIZED_DATE = '2026-10-16'\nREVIEW_ONLY = False\n"
        copies, report = stage.stage_sources(sources, {})
        for name in ("runtime.py", "short_runtime.py"):
            self.assertIn("AUTHORIZED_DATE = '2026-10-21'", copies[name])
            self.assertIn("REVIEW_ONLY = True", copies[name])
        self.assertIs(report["can_arm"], False)

    def test_calendar_and_invalid_syntax_refused(self):
        for old, new in (("2026-10-19", "2026-10-22"), ("SOURCE =", "SOURCE (=")):
            sources = self.sources()
            sources["runtime.py"] = sources["runtime.py"].replace(old, new)
            with self.subTest(new=new), self.assertRaises(ValueError):
                stage.stage_sources(sources, {})


if __name__ == "__main__":
    unittest.main()
