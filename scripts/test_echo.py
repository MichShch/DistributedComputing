#!/usr/bin/env python3
'''Simple echo script for agent checks. Prints all provided command-line arguments.'''

import sys


def main() -> int:
    args = sys.argv[1:]
    print('test-echo: running')
    print(f'args count: {len(args)}')

    for idx, arg in enumerate(args, start=1):
        print(f'arg{idx}: {arg}')

    print('test-echo: done')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
