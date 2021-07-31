#!/usr/bin/env python3
# xxdi.py - Pure Python3 implementation of 'xxd -i [input] [output] with zlib compression'
import sys
from functools import partial
from os.path import basename

import zlib

count = 0
compression_level = 9 # Best Compression
mem_level = 9 # Best speed and compression
strategy = zlib.Z_DEFAULT_STRATEGY
wbits = zlib.MAX_WBITS # zlib

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

gzip_compress_obj = zlib.compressobj(compression_level,
                                     zlib.DEFLATED, wbits, mem_level, strategy)

uncompressed_data = fd_in.read()
gzipped_data = gzip_compress_obj.compress(uncompressed_data)
gzipped_data += gzip_compress_obj.flush()

fd_out.write('    ')
for byte in gzipped_data:
    if (count + 1 == len(gzipped_data)):
        print("0x%02x" % byte, end='\n};\n', file=fd_out)
    elif (count + 1) % 13:
        print("0x%02x" % byte, end=', ', file=fd_out)
    else:
        print("0x%02x" % byte, end=',\n    ', file=fd_out)
    count += 1;

print('static const size_t %s_len = %d;' % (target_name, count), file=fd_out)

fd_in.close()
