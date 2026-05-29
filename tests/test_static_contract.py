from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class StaticContractTest(unittest.TestCase):
    def test_benchmark_exposes_required_copy_modes(self):
        source = read("src/ascend_d2h_bench.cpp")
        for mode in ["async-loop", "batch", "sync", "all"]:
            self.assertIn(mode, source)

    def test_benchmark_exposes_required_allocator_modes(self):
        source = read("src/ascend_d2h_bench.cpp")
        for allocator in ["aclrt-malloc-host", "ucm-direct", "register-pinned"]:
            self.assertIn(allocator, source)

    def test_readme_documents_core_parameters(self):
        readme = read("README.md")
        for option in [
            "--mode",
            "--allocator",
            "--io-size",
            "--io-count",
            "--tensor-size-list",
            "--streams",
            "--iters",
        ]:
            self.assertIn(option, readme)

    def test_benchmark_exposes_fragmented_tensor_size_list(self):
        source = read("src/ascend_d2h_bench.cpp")
        readme = read("README.md")
        self.assertIn("--tensor-size-list", source)
        self.assertIn("131072,16384,32768", readme)

    def test_bandwidth_unit_is_bytes_not_bits(self):
        source = read("src/ascend_d2h_bench.cpp")
        readme = read("README.md")
        self.assertIn("bandwidth_avg_GBps", source)
        self.assertIn("GB/s", readme)


if __name__ == "__main__":
    unittest.main()
