# Fields, NF, $NF, FS changed in BEGIN and per record, OFS on rebuild.
BEGIN { FS = ":" }
{ print NR ": " $1 " has " NF " fields, last " $NF }
NR == 2 { $2 = "CHANGED"; OFS = "-"; $1 = $1; print }
END { print "records", NR }
