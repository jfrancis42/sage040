# Case conversion and . over UTF-8, in a UTF-8 locale.
s/.*/\U&/
s/^\(.\)\(.\)/\2\1/
s/./[&]/5
