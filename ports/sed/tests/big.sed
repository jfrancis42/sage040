# Volume: 3000 lines, every one rewritten, a total kept in hold.
s/w\([0-9]*\)/W\1/g
/^0[0-9]*0 /{
H
}
$!d
x
s/\n/|/g
s/^\(.\{200\}\).*/\1/
