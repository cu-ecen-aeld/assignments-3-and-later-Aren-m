#!/bin/sh

if [ $# -lt 2 ]
then
	echo "script requires 2 arguments"
	exit 1
else
	writefile=$1
	writestr=$2
	echo "$writestr" > "$writefile"
	
	if [ ! -f "$writefile" ]
	then
		echo "file could not be created"
		exit 1
	fi
fi
