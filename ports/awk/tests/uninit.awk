# Uninitialized values, numeric strings from input, and comparisons.
{ print ($1 < $2) ? "less" : "not-less", $1 + 0, ($1 == $1 + 0) }
END { print u + 0, "[" u "]", length(u), (u == 0), (u == "") }
