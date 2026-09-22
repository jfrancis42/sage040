# Regular expressions: classes, anchors, alternation, dynamic regexes.
{
    printf "%-12s", $0
    printf " digit=%d", ($0 ~ /^[[:digit:]]+$/)
    printf " alpha=%d", ($0 ~ /^[[:alpha:]]+$/)
    printf " alt=%d", ($0 ~ /^(cat|dog)s?$/)
    re = "^[a-c]+x*$"
    printf " dyn=%d", ($0 ~ re)
    printf " dot=%d", ($0 ~ /^.\..$/)
    printf " rep=%d\n", ($0 ~ /^a{2,3}$/)
}
