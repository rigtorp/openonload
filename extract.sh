#!/bin/sh

set -ex

for FILE in "$@"
do
	rm -rf *
	tar xf "$FILE" --strip-components 1
	git add -A
	git commit -m $(basename "$FILE")
done
