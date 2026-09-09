#!/usr/bin/env python3
"""Prepare the official SDK sizeof/offsetof probe from downloaded public INIs."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    root = parser.parse_args().directory
    tables = []
    for filename, stream, prefix in [
        ('ordLog_trades.ini', 'FORTS_ORDLOG_REPL', 'Ordlog'),
        ('ordbook.ini', 'FORTS_ORDBOOK_REPL', 'Ordbook'),
    ]:
        raw = (root / filename).read_bytes()
        source = raw.decode('utf-8-sig').replace('\r\n', '\n')
        for index, name in enumerate(re.findall(r'^table=(\w+)', source, re.M)):
            block = source.split('[table:CustReplScheme:' + name + ']', 1)[1].split('[table:', 1)[0]
            fields = re.findall(r'^field=([^,\n]+),([^,\n]+)', block, re.M)
            tables.append(dict(
                stream=stream, scheme='CustReplScheme', table=name, index=index, msg_id=0,
                source_file=filename, source_sha256=hashlib.sha256(raw).hexdigest(), namespace=prefix,
                fields=[dict(name=field, type=kind) for field, kind in fields],
            ))
    code = [
        '#include <cgate.h>', '#include <cstdio>', '#include <cstddef>',
        'namespace Ordlog {', '#include "ordlog-official.h"', '}', '#undef _CustReplScheme_H_',
        'namespace Ordbook {', '#include "ordbook-official.h"', '}', 'int main() {',
    ]
    for table in tables:
        qualified = table['namespace'] + '::' + table['table']
        code.append(f'printf("T,{qualified},%zu,%zu\\n",sizeof({qualified}),{qualified}_index);')
        for field in table['fields']:
            name = field['name']
            code.append(
                f'printf("F,{qualified},{name},%zu,%zu\\n",'
                f'offsetof({qualified},{name}),sizeof((({qualified}*)nullptr)->{name}));'
            )
    code.extend(['printf("TIME,%zu,%zu\\n",sizeof(cg_time_t),offsetof(cg_time_t,msec));', '}'])
    (root / 'layout-probe.cpp').write_text('\n'.join(code) + '\n')
    (root / 'parsed-tables.json').write_text(json.dumps(tables, indent=2) + '\n')


if __name__ == '__main__':
    main()
