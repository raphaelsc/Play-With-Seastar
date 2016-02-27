#!/bin/bash

file=./libseastar.a
compressed_file=./libseastar.a.gz

if [ -e "$file" ]; then
    echo "libseastar.a exists, no need for decompression"
else
    echo "decompressing libseastar.a.gz"
    gzip -c -d $compressed_file > $file
fi

