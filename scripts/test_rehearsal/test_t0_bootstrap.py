import copy
import unittest
from t0_bootstrap import verify_build, stage_bootstrap, SOURCE, BINARY, ARCHIVE, BUILD

class ExactBuild(unittest.TestCase):
    def setUp(self):
        self.receipt = dict(source_git_sha=SOURCE,state='BUILD_PASS',candidate_binary_sha256=BINARY,
            source_archive_sha256=ARCHIVE,trading_armed=False,
            release_tests=dict(passed=39,tests=39,failed=0,skipped=0))
        self.inputs = dict(attempt=BUILD,binary_sha256=BINARY,archive_sha256=ARCHIVE)

    def test_actual_candidate_receipt_without_relabelling_37_tests(self):
        before=copy.deepcopy(self.receipt)
        self.assertTrue(verify_build(self.receipt,self.inputs,BINARY))
        self.assertEqual(before,self.receipt)

    def test_wrong_source_binary_archive_or_test_result_refuses(self):
        for key,value in [('source_git_sha','0'*40),('candidate_binary_sha256','0'*64),
                          ('source_archive_sha256','0'*64),('state','complete'),('trading_armed',True)]:
            with self.subTest(key=key):
                d=copy.deepcopy(self.receipt);d[key]=value
                with self.assertRaises(ValueError):verify_build(d,self.inputs,BINARY)
        for key,value in [('passed',37),('tests',38),('failed',1),('skipped',1),('passed',True)]:
            d=copy.deepcopy(self.receipt);d['release_tests'][key]=value
            with self.assertRaises(ValueError):verify_build(d,self.inputs,BINARY)

    def test_bootstrap_inputs_cannot_adopt_inherited_native_build(self):
        for key in self.inputs:
            d=dict(self.inputs);d[key]='stale'
            with self.assertRaises(ValueError):verify_build(self.receipt,d,BINARY)
        with self.assertRaises(ValueError):verify_build(self.receipt,self.inputs,'0'*64)

    def test_unknown_source_template_refuses_instead_of_partial_rewrite(self):
        with self.assertRaisesRegex(ValueError,'predecessor_changed'):stage_bootstrap('def setup(): pass')

if __name__=='__main__':unittest.main()
