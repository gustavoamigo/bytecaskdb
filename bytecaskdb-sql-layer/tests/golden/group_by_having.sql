SELECT w, sum(v), avg(v) FROM p WHERE a = $1 GROUP BY w HAVING count(*) > $2
