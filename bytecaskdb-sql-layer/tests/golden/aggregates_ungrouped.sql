SELECT count(*), max(v), avg(v) FROM p WHERE a = $1 HAVING count(*) > $2
