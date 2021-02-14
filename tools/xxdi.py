#!/usr/bin/env python3
# xxdi.py - Pure Python3 implementation of 'xxd -i [input] [output]'
import sys
from functools import partial
from os.path import basename

count = 0

# Convert '/', '$' and '.' to '_',
# or if this is stdin just use "stdin" as the name.
argc = len(sys.argv)
if argc == 1:
    fd_in = sys.stdin.buffer
    target_name = 'stdin'
    fd_out = sys.stdout
elif argc == 2 or argc == 3:
    fd_in = open(sys.argv[1], "rb")
    target_name = basename(sys.argv[1]).replace('/','_').replace('$','_').replace('.','_')
    if argc == 3:
        fd_out = open(sys.argv[2], "w+")
    else:
        fd_out = sys.stdout
else:
	print("Too many arguments!", file=sys.stderr)
	print("Usage: xxdi.py [input] [output]", file=sys.stderr)
	exit(1)

print("#pragma once\n#include <stdint.h>", file=fd_out)
print("static const uint8_t %s[] = {" % target_name, file=fd_out)

for block in iter(partial(fd_in.read, 12), b''):
    fd_out.write('\t')
    for i in block:
        print("0x%02x" % i, end=', ', file=fd_out)
        count += 1;
    fd_out.write('\n')

print('};', file=fd_out)

print('static const size_t %s_len = %d;' % (target_name, count), file=fd_out)

fd_in.close()
