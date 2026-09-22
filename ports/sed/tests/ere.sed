# Extended regexes (-E): groups, alternation, repetition, classes.
s/^(alpha|gamma) ([0-9]+)$/\2:\1/
s/([a-z]+) ([0-9]{2})$/\1=\2/
/^[[:alpha:]]+$/s//<&>/
