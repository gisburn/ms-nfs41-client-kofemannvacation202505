#!/bin/bash

#
# MIT License
#
# Copyright (c) 2023-2025 Roland Mainz <roland.mainz@nrubsig.org>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
#

#
# wintartest_comparewinvsgnu001.bash - filesystem test
# which compares files unpacked by /cygdrive/c/Windows/system32/tar
# and /usr/bin/tar, and checks whether files have same hashes
#
# Written by Roland Mainz <roland.mainz@nrubsig.org>
#

#
# Notes:
# - Genreate test tar.bz2 files like this:
# $ seq 1000000 >10000seq.txt ; tar -cvf - 10000seq.txt | bzip2 -9 >10000seq.tar.bz2
# - Compare individual files with
# $ diff -u <(od -x -v cygwintar/bin/ksh93.exe) <(od -x -v wintar/bin/ksh93.exe)
#

export PATH='/bin:/usr/bin'

# set umask=0000 to avoid permission madness on SMB
umask 0000

typeset intarfile="$1"
typeset f
typeset -i num_failed_hash_compare=0

if [[ ! -x '/cygdrive/c/Windows/system32/tar' ]] ; then
	printf $"%s: %s not found.\n" \
		"$0" '/cygdrive/c/Windows/system32/tar' 1>&2
	exit 1
fi

if [[ ! -r "$intarfile" ]] ; then
	printf $"%s: Input file %q not readable.\n" \
		"$0" "$intarfile" 1>&2
	exit 1
fi

typeset IFS=$'\n'

set -o xtrace
set -o errexit

intarfile="$(realpath "$intarfile")"

rm -Rf 'wintar_tmp' 'cygwintar_tmp'
mkdir 'wintar_tmp' 'cygwintar_tmp'

cd 'wintar_tmp'
/cygdrive/c/Windows/system32/tar -xvf "$(cygpath -w "$intarfile")"
cd '..'

cd 'cygwintar_tmp'
/usr/bin/tar -xvf "$intarfile"
cd '..'

typeset -a file_list=(
	$(cd 'cygwintar_tmp' && find . -type f)
)

set +o xtrace +o errexit

for f in "${file_list[@]}" ; do
	IFS=' ' read wintar_hash dummy < <(openssl md5 -r "wintar_tmp/$f")
	IFS=' ' read cygwintar_hash dummy < <(openssl md5 -r "cygwintar_tmp/$f")

	if [[ "$wintar_hash" == "$cygwintar_hash" ]] ; then
		printf $"NOTE:\tHashes for file %q OK\n" "$f"
	else
		printf $"ERROR:\tHashes for file %q differ, wintar_hash(%s) != cygwintar_hash(%s)\n" \
			"$f" "$wintar_hash" "$cygwintar_hash"
		(( num_failed_hash_compare++ ))
	fi
done

exit $((num_failed_hash_compare > 0?1:0))
