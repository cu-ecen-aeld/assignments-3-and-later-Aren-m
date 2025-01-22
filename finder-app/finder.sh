#!/bin/sh

if [ $# -lt 2 ]
then
	echo "the script requires 2 arguments"
	exit 1
else
	filesdir=$1
	searchstr=$2
	
	if [ -d "$filesdir" ] 
	then
		X=$(ls $filesdir | wc -l)
		Y=$(grep -r $searchstr $filesdir | wc -l)
		echo "The number of files are ${X} and the number of matching lines are ${Y}"
	else
		echo "first argument must be a valid directory"
		exit 1
	fi
fi
