"""Compare runtime text conversion against Python codecs and strict JSON."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="moex-text-") as directory:
    path = Path(directory) / "evidence.json"
    result = subprocess.run([sys.argv[1], str(path)], check=True, capture_output=True)
    mappings = result.stdout.decode("utf-8", errors="strict").splitlines()
    assert len(mappings) == 256
    for byte, encoded in enumerate(mappings):
        assert json.loads(encoded) == bytes([byte]).decode("cp1251", errors="replace"), byte
    document = json.loads(path.read_bytes().decode("utf-8", errors="strict"))
    assert document["name"] == "Фьючерсный контракт ALRS-12.26"
    assert json.loads(json.dumps(document, ensure_ascii=False).encode("utf-8")) == document
    runner_receipt = json.loads(Path(str(path) + ".runner.json").read_bytes().decode("utf-8", errors="strict"))
    assert runner_receipt["metadata_507_ready"] is False
    assert runner_receipt["symbol"] == "А \"quoted\" \\\x00\x01\x1f\ufffd"
    assert runner_receipt["invalid_utf8_raw_hex"] == {"symbol": "ff"}
