# mailcheck
mfck :: fsck = mbox files :: file systems

mfck is a mailbox file checking tool.  It will allow you to check your mbox files' integrity, examine their contents, and optionally
perform automatic repairs.

Usage: mfck [-acdfhinopqruvN] \<mbox\> ...

Option		| Description
----------------|-----------------------------------------------------------
 -b		| backup mbox to mbox~ before changing it
 -c 		| check the mbox for consistency
 -d 		| debug mode (see source code)
 -f \<file\> 	| process mbox \<file\>
 -h 		| print out this help text
 -i 		| initiate interactive mode
 -n 		| dry run -- no changes will be made to any file
 -o \<file\> 	| concatenate messages into \<file\>
 -q 		| be quiet and don't report warnings or notices
 -r 		| repair the given mailboxes
 -s 		| be stringent and report more indiscretions than otherwise
 -u 		| unique messages in each mailbox by removing duplicates
 -v 		| be verbose and print out more progress information
 -C 		| show a few lines of context around parse errors
 -N 		| don't try to mmap the mbox file
 -V 		| print out mfck version information and then exit

If given no options, mfck will simply to try read the given mbox files
and then quit. More interesting usage examples would be:

Example		| Description
----------------|-----------------------------------------------
`mfck -c mbox`	| check the mbox file and report most errors
`mfck -cs mbox`	| check the mbox file and report more errors
`mfck -rb mbox`	| check the mbox, perform any necessary repairs, and save the original file as mbox~
`mfck -ci mbox`	| check the mbox and then enter an interactive mode where you can further inspect it

If you just want to test things out without making any changes, add the -n flag and no files will be modified.
