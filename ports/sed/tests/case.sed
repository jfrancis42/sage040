# \u \l \U \L \E in replacements, and the M (multiline) flag.
1s/\(a\)\(b\)\(c\)/\u\1\U\2\E\3/
2s/\(A\)\(B\)\(C\)/\l\1\L\2\E\3/
N
s/^ABC$/<&>/M
