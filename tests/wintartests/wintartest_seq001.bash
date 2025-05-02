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
# wintartest_seq001.bash - filesystem test
# which generates files with /usr/bin/seq, packs them with
# Cygwin's GNU /usr/bin/tar and tests whether they contain
# zero-bytes (which should never appear in a text file)
# after unpacking them
# with /cygdrive/c/Windows/system32/tar
#
# Written by Roland Mainz <roland.mainz@nrubsig.org>
#

function test_wintar_seq
{
	set -o xtrace
	set -o errexit
	set -o nounset

	# config
	typeset use_bzip2=$1
	typeset use_localdiskfortar=$2

	# local vars
	typeset tarfile_dir
	typeset tarfilename
	typeset -i i
	typeset out
	typeset -a testfiles
	typeset currf

	# seq 1040 == 4093 bytes
	# seq 1042 == 4103 bytes
	for i in 1 100 1040 5000 10000 12000 ; do
		rm -f -- "${i}seq.txt"
		seq "$i" >"${i}seq.txt"
		testfiles+=( "${i}seq.txt" )
	done

	if "${use_localdiskfortar}" ; then
		tarfile_dir='/tmp'
	else
		tarfile_dir="$PWD"
	fi

	if ${use_bzip2} ; then
		tarfilename="${tarfile_dir}/test_seq.tar.bz2"
		tar -cvf - "${testfiles[@]}" | pbzip2 -1 >"${tarfilename}"
	else
		tarfilename="${tarfile_dir}/test_seq.tar"
		tar -cvf - "${testfiles[@]}" >"${tarfilename}"
	fi

	rm -Rf 'tmp'
	mkdir 'tmp'
	cd 'tmp'

	set +o xtrace

	for (( i=0 ; i < 2000 ; i++ )) ; do
		printf '#### Test cycle %d (usingbzip=%s,tarfileonlocaldisk=%s):\n' "$i" "$use_bzip2" "$use_localdiskfortar"
		/cygdrive/c/Windows/system32/tar -xvf "$(cygpath -w "${tarfilename}")"

		for currf in "${testfiles[@]}" ; do
			if [[ ! -r "$currf" ]] ; then
				printf '## ERROR: File %q not found.\n' "$currf"
				return 1
			fi
			if [[ ! -s "$currf" ]] ; then
				printf '## ERROR: File %q is empty (ls -l == "%s").\n' "$currf" "$(ls -l "$currf")"
				return 1
			fi

			out="$(od -A x -t x1 -v "$currf" | grep -F ' 00' | head -n 5)"

			if [[ "$out" != '' ]] ; then
				printf '## ERROR: Zero byte in plain /usr/bin/seq output %q found:\n' "$currf"
				printf -- '---- snip ----\n%s\n---- snip ----\n' "$out"
				return 1
			fi
		done

		rm -f -- "${testfiles[@]}"
	done

	printf '##### SUCCESS\n'

	return 0
}


#
# main
#

export PATH='/bin:/usr/bin'

if [[ ! -x '/cygdrive/c/Windows/system32/tar' ]] ; then
	printf $"%s: %s not found.\n" \
		"$0" '/cygdrive/c/Windows/system32/tar' 1>&2
	exit 1
fi

# Set umask=0000 to avoid permission trouble on SMB filesystems
umask 0000

test_wintar_seq true true
exit $?
# EOF.
