SELECT x.id, y.id FROM t x JOIN t y ON y.id = x.qty WHERE x.name = $1
