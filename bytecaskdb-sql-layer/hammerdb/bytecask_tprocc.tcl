#!/usr/local/bin/tclsh9.0
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# bytecask_tprocc.tcl — HammerDB's TPROC-C as a custom script
# (docs/server_design.md, *The HammerDB driver*): a port of the client-side
# logic of HammerDB's MySQL driver (src/mysql/mysqloltp.tcl, its
# mysql_no_stored_procs procedures and timed monitor) to pgtcl, sending the
# statements of docs/frontend_design.md, which tests/proof/test_tprocc.py
# checks against PostgreSQL 17. scripts/check_driver_statements.py fails
# CI if a statement here is not one of those.
#
# Run by hammerdb/run-gate.sh, which writes the OPTIONS below: hammerdbcli
# with `dbset db pg`, `customscript` this file and `custommonitor timed`.
# Virtual user 1 is the monitor; the others run transactions.
#OPTIONS
set total_iterations 10000000 ;# Transactions per virtual user before it stops
set RAISEERROR "false" ;# Stop a virtual user at its first error (true or false)
set rampup 2 ;# Minutes before the first count
set duration 5 ;# Minutes between the two counts
set host "127.0.0.1" ;# sql_layer_server's address
set port "5432" ;# and port
set user "tprocc" ;# Any name: trust authentication
set db "tprocc" ;# The database name the server serves
set warehouses 1 ;# Warehouses loaded
set run 1 ;# The harness_run the run script took for this cell: the h_ids' top bits
set mix {neword payment ostat delivery} ;# Transaction types to run; slev once front-end slice 5 lands
set engine "ByteCaskDB" ;# The name HammerDB's result line gives the system
#OPTIONS

#LOAD LIBRARIES AND MODULES
if [catch {package require Pgtcl} message] { error "Failed to load Pgtcl - $message" }
if [catch {package require tpcccommon} ] { error "Failed to load HammerDB's tpcccommon functions" } else { namespace import tpcccommon::* }

if { [ chk_thread ] eq "FALSE" } {
    error "The TPROC-C timed script must run in a thread-enabled interpreter"
}

# The statements, one per line, global: a virtual user's top level may not
# be. check_driver_statements.py reads them here.
set ::SQL(P1) {UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2}
set ::SQL(P2) {SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = $1}
set ::SQL(P3) {UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3}
set ::SQL(P4) {SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM district WHERE d_w_id = $1 AND d_id = $2}
set ::SQL(P5) {SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3}
set ::SQL(P6) {SELECT c_first, c_middle, c_id, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_last = $3 ORDER BY c_first}
set ::SQL(P7) {SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3}
set ::SQL(P8) {SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3}
set ::SQL(P9) {UPDATE customer SET c_balance = $1, c_data = $2 WHERE c_w_id = $3 AND c_d_id = $4 AND c_id = $5}
set ::SQL(P10) {UPDATE customer SET c_balance = $1 WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4}
set ::SQL(P11) {INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, h_data) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)}
set ::SQL(O2) {SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3 ORDER BY c_first}
set ::SQL(O3) {SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_id = $1 AND c_d_id = $2 AND c_w_id = $3}
set ::SQL(O4) {SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = $1 AND o_w_id = $2 AND o_c_id = $3 ORDER BY o_id DESC LIMIT 1}
set ::SQL(O5) {SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3}
set ::SQL(N1) {SELECT c_discount, c_last, c_credit, w_tax FROM customer, warehouse WHERE w_id = $1 AND c_w_id = $1 AND c_d_id = $2 AND c_id = $3}
set ::SQL(N2) {SELECT d_next_o_id, d_tax FROM district WHERE d_id = $1 AND d_w_id = $2 FOR UPDATE}
set ::SQL(N3) {UPDATE district SET d_next_o_id = d_next_o_id + 1 WHERE d_id = $1 AND d_w_id = $2}
set ::SQL(N4) {SELECT i_price, i_name, i_data FROM item WHERE i_id = $1}
set ::SQL(N5) {SELECT s_quantity, s_data, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10 FROM stock WHERE s_i_id = $1 AND s_w_id = $2}
set ::SQL(N6) {UPDATE stock SET s_quantity = $1 WHERE s_i_id = $2 AND s_w_id = $3}
set ::SQL(N7) {INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_dist_info) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)}
set ::SQL(N8) {INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_ol_cnt, o_all_local) VALUES ($1, $2, $3, $4, $5, $6, $7)}
set ::SQL(N9) {INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1, $2, $3)}
set ::SQL(D1) {SELECT no_o_id FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 ORDER BY no_o_id LIMIT 1}
set ::SQL(D2) {DELETE FROM new_order WHERE no_w_id = $1 AND no_d_id = $2 AND no_o_id = $3}
set ::SQL(D3) {SELECT o_c_id FROM orders WHERE o_id = $1 AND o_d_id = $2 AND o_w_id = $3}
set ::SQL(D4) {UPDATE orders SET o_carrier_id = $1 WHERE o_id = $2 AND o_d_id = $3 AND o_w_id = $4}
set ::SQL(D5) {UPDATE order_line SET ol_delivery_d = $1 WHERE ol_o_id = $2 AND ol_d_id = $3 AND ol_w_id = $4}
set ::SQL(D6) {SELECT SUM(ol_amount) FROM order_line WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3}
set ::SQL(D7) {UPDATE customer SET c_balance = c_balance + $1 WHERE c_id = $2 AND c_d_id = $3 AND c_w_id = $4}
set ::SQL(S1) {SELECT d_next_o_id FROM district WHERE d_w_id = $1 AND d_id = $2}
set ::SQL(S2) {SELECT COUNT(DISTINCT s_i_id) FROM order_line, stock WHERE ol_w_id = $1 AND ol_d_id = $2 AND ol_o_id < $3 AND ol_o_id >= $3 - 20 AND s_w_id = $1 AND s_i_id = ol_i_id AND s_quantity < $4}

proc connect { host port user db } {
    return [ pg_connect -conninfo "host=$host port=$port user=$user dbname=$db sslmode=disable gssencmode=disable" ]
}

# One statement, as PQexecParams sends it with parameters and PQexec
# without: its rows as a list of lists. A refusal is a Tcl error whose
# errorCode is {PG sqlstate}.
proc q { conn name args } {
    set r [ pg_exec $conn $::SQL($name) {*}$args ]
    if { [ pg_result $r -status ] eq "PGRES_FATAL_ERROR" } {
        set state [ pg_result $r -error sqlstate ]
        set message [ pg_result $r -error ]
        pg_result $r -clear
        error "$name: $message" {} [ list PG $state ]
    }
    set rows {}
    for {set i 0} {$i < [ pg_result $r -numTuples ]} {incr i} { lappend rows [ pg_result $r -getTuple $i ] }
    pg_result $r -clear
    return $rows
}

proc control { conn word } {
    set r [ pg_exec $conn $word ]
    set status [ pg_result $r -status ]
    set state [ pg_result $r -error sqlstate ]
    set message [ pg_result $r -error ]
    pg_result $r -clear
    if { $status eq "PGRES_FATAL_ERROR" } { error "$word: $message" {} [ list PG $state ] }
}

# Adds one to a shared counter, creating it at 0 the first time.
proc count { array key } {
    tsv::lock $array {
        if { ![ tsv::exists $array $key ] } { tsv::set $array $key 0 }
        tsv::incr $array $key
    }
}

# A transaction the client rolls back on purpose (New-Order's unknown item).
proc client_rollback {} { error "rolled back by the client" {} ROLLBACK }

# Runs `body` between BEGIN and COMMIT and counts its one outcome: committed,
# rolled back (by the client) or failed (an error, counted by its SQLSTATE).
# A failed transaction is not retried, as HammerDB's drivers do not retry.
proc transaction { conn raise body } {
    if { [ catch { control $conn BEGIN; uplevel 1 $body; control $conn COMMIT } message options ] } {
        catch { control $conn ROLLBACK }
        set code [ dict get $options -errorcode ]
        if { $code eq "ROLLBACK" } {
            count counts rolled_back
            return
        }
        count counts failed
        count errors [ expr { [ lindex $code 0 ] eq "PG" ? [ lindex $code 1 ] : "client" } ]
        if { $raise } { error $message } else { puts $message }
        return
    }
    count counts committed
}

proc now {} { return [ clock format [ clock seconds ] -format "%Y-%m-%d %H:%M:%S" ] }
proc money { v } { return [ format %.2f $v ] }

# The middle customer of those a last name selects, ceil(n / 2), as the
# specification's profile has it; "" when there is none.
proc middle { rows n } {
    if { $n == 0 } { return "" }
    return [ lindex $rows [ expr { ($n + 1) / 2 - 1 } ] ]
}

#PAYMENT
proc payment { conn p_w_id w_id_input hid } {
    set p_d_id [ RandomNumber 1 10 ]
    set x [ RandomNumber 1 100 ]
    set y [ RandomNumber 1 100 ]
    if { $x <= 85 } {
        set p_c_d_id $p_d_id
        set p_c_w_id $p_w_id
    } else {
        set p_c_d_id [ RandomNumber 1 10 ]
        set p_c_w_id [ RandomNumber 1 $w_id_input ]
        while { ($p_c_w_id == $p_w_id) && ($w_id_input != 1) } { set p_c_w_id [ RandomNumber 1 $w_id_input ] }
    }
    set name [ randname [ NURand 255 0 999 123 ] ]
    set p_c_id [ RandomNumber 1 3000 ]
    set amount [ RandomNumber 1 5000 ]
    set h_date [ now ]
    q $conn P1 $amount $p_w_id
    set w_name [ lindex [ lindex [ q $conn P2 $p_w_id ] 0 ] 5 ]
    q $conn P3 $amount $p_w_id $p_d_id
    set d_name [ lindex [ lindex [ q $conn P4 $p_w_id $p_d_id ] 0 ] 5 ]
    if { $y <= 60 } {
        set n [ lindex [ lindex [ q $conn P5 $name $p_c_d_id $p_c_w_id ] 0 ] 0 ]
        set row [ middle [ q $conn P6 $p_c_w_id $p_c_d_id $name ] $n ]
        if { $row eq "" } { client_rollback }
        set p_c_id [ lindex $row 2 ]
    } else {
        set row [ lindex [ q $conn P7 $p_c_w_id $p_c_d_id $p_c_id ] 0 ]
    }
    set credit [ lindex $row 9 ]
    set balance [ money [ expr { [ lindex $row 12 ] - $amount } ] ]
    if { $credit eq "BC" } {
        set c_data [ lindex [ lindex [ q $conn P8 $p_c_w_id $p_c_d_id $p_c_id ] 0 ] 0 ]
        set c_data [ string range "$p_c_id $p_c_d_id $p_c_w_id $p_d_id $p_w_id [ money $amount ] | $c_data" 0 499 ]
        q $conn P9 $balance $c_data $p_c_w_id $p_c_d_id $p_c_id
    } else {
        q $conn P10 $balance $p_c_w_id $p_c_d_id $p_c_id
    }
    q $conn P11 $hid $p_c_d_id $p_c_w_id $p_c_id $p_d_id $p_w_id $h_date $amount "$w_name    $d_name"
}

#ORDER STATUS
proc ostat { conn w_id } {
    set d_id [ RandomNumber 1 10 ]
    set name [ randname [ NURand 255 0 999 123 ] ]
    set c_id [ RandomNumber 1 3000 ]
    if { [ RandomNumber 1 100 ] <= 60 } {
        set n [ lindex [ lindex [ q $conn P5 $name $d_id $w_id ] 0 ] 0 ]
        set row [ middle [ q $conn O2 $name $d_id $w_id ] $n ]
        if { $row eq "" } { return }
        set c_id [ lindex $row 3 ]
    } else {
        q $conn O3 $c_id $d_id $w_id
    }
    set orders [ q $conn O4 $d_id $w_id $c_id ]
    if { [ llength $orders ] } { q $conn O5 [ lindex [ lindex $orders 0 ] 0 ] $d_id $w_id }
}

#NEW ORDER
proc neword { conn no_w_id w_id_input } {
    set d_id [ RandomNumber 1 10 ]
    set c_id [ RandomNumber 1 3000 ]
    set ol_cnt [ RandomNumber 5 15 ]
    set rbk [ RandomNumber 1 99 ]
    set date [ now ]
    # The lines first, so o_all_local is known when the order is inserted.
    set lines {}
    set all_local 1
    for {set number 1} {$number <= $ol_cnt} {incr number} {
        set i_id [ expr { ($number == $ol_cnt && $rbk == 1) ? 100001 : [ RandomNumber 1 100000 ] } ]
        set supply $no_w_id
        if { [ RandomNumber 1 100 ] == 1 && $w_id_input != 1 } {
            set all_local 0
            while { $supply == $no_w_id } { set supply [ RandomNumber 1 $w_id_input ] }
        }
        lappend lines [ list $number $i_id $supply [ RandomNumber 1 10 ] ]
    }
    lassign [ lindex [ q $conn N1 $no_w_id $d_id $c_id ] 0 ] discount last credit w_tax
    lassign [ lindex [ q $conn N2 $d_id $no_w_id ] 0 ] o_id d_tax
    q $conn N3 $d_id $no_w_id
    q $conn N8 $o_id $d_id $no_w_id $c_id $date $ol_cnt $all_local
    q $conn N9 $o_id $d_id $no_w_id
    foreach line $lines {
        lassign $line number i_id supply quantity
        set item [ q $conn N4 $i_id ]
        if { ![ llength $item ] } { client_rollback }
        set price [ lindex [ lindex $item 0 ] 0 ]
        set stock [ lindex [ q $conn N5 $i_id $supply ] 0 ]
        set s_quantity [ lindex $stock 0 ]
        # The specification's 2.4.2.2: restocked by 91 below 10 left.
        if { $s_quantity >= $quantity + 10 } {
            set s_quantity [ expr { $s_quantity - $quantity } ]
        } else {
            set s_quantity [ expr { $s_quantity - $quantity + 91 } ]
        }
        q $conn N6 $s_quantity $i_id $supply
        set amount [ money [ expr { $quantity * $price * (1 + $w_tax + $d_tax) * (1 - $discount) } ] ]
        q $conn N7 $o_id $d_id $no_w_id $number $i_id $supply $quantity $amount [ lindex $stock [ expr { $d_id + 1 } ] ]
    }
}

#DELIVERY
proc delivery { conn w_id } {
    set carrier [ RandomNumber 1 10 ]
    set date [ now ]
    for {set d_id 1} {$d_id <= 10} {incr d_id} {
        set oldest [ q $conn D1 $w_id $d_id ]
        if { ![ llength $oldest ] } { continue }  ;# the district has no undelivered order
        set o_id [ lindex [ lindex $oldest 0 ] 0 ]
        q $conn D2 $w_id $d_id $o_id
        set c_id [ lindex [ lindex [ q $conn D3 $o_id $d_id $w_id ] 0 ] 0 ]
        q $conn D4 $carrier $o_id $d_id $w_id
        q $conn D5 $date $o_id $d_id $w_id
        set total [ lindex [ lindex [ q $conn D6 $o_id $d_id $w_id ] 0 ] 0 ]
        if { $total eq "" } { set total 0 }
        q $conn D7 $total $c_id $d_id $w_id
    }
}

#STOCK LEVEL
proc slev { conn w_id d_id } {
    set threshold [ RandomNumber 10 20 ]
    set next_o_id [ lindex [ lindex [ q $conn S1 $w_id $d_id ] 0 ] 0 ]
    q $conn S2 $w_id $d_id $next_o_id $threshold
}

# The sum of every district's d_next_o_id, in one transaction so it is one
# snapshot: S1 per district, the layer having no full-table SUM.
proc next_order_ids { conn warehouses } {
    control $conn BEGIN
    set sum 0
    for {set w 1} {$w <= $warehouses} {incr w} {
        for {set d 1} {$d <= 10} {incr d} {
            incr sum [ lindex [ lindex [ q $conn S1 $w $d ] 0 ] 0 ]
        }
    }
    control $conn COMMIT
    return $sum
}

proc transactions_so_far {} {
    set total 0
    foreach k {committed rolled_back failed} {
        if { [ tsv::exists counts $k ] } { incr total [ tsv::get counts $k ] }
    }
    return $total
}

# HammerDB's timed monitor, with NOPM from d_next_o_id and TPM from the
# users' own counts.
proc monitor { host port user db rampup duration warehouses totalvirtualusers engine } {
        set conn [ connect $host $port $user $db ]
        puts "Beginning rampup time of $rampup minutes"
        set ramptime 0
        set rampup [ expr { $rampup * 60000 } ]
        while { $ramptime != $rampup } {
            if { [ tsv::get application abort ] } { break } else { after 6000 }
            set ramptime [ expr { $ramptime + 6000 } ]
            if { ![ expr { $ramptime % 60000 } ] } { puts "Rampup [ expr { $ramptime / 60000 } ] minutes complete ..." }
        }
        if { [ tsv::get application abort ] } { return }
        puts "Rampup complete, Taking start Transaction Count."
        set start_trans [ transactions_so_far ]
        set start_nopm [ next_order_ids $conn $warehouses ]
        puts "Timing test period of $duration in minutes"
        set testtime 0
        set durmin $duration
        set duration [ expr { $duration * 60000 } ]
        while { $testtime != $duration } {
            if { [ tsv::get application abort ] } { break } else { after 6000 }
            set testtime [ expr { $testtime + 6000 } ]
            if { ![ expr { $testtime % 60000 } ] } { puts -nonewline "[ expr { $testtime / 60000 } ]  ...," }
        }
        if { [ tsv::get application abort ] } { return }
        puts "Test complete, Taking end Transaction Count."
        set end_trans [ transactions_so_far ]
        set end_nopm [ next_order_ids $conn $warehouses ]
        set tpm [ expr { ($end_trans - $start_trans) / $durmin } ]
        set nopm [ expr { ($end_nopm - $start_nopm) / $durmin } ]
        puts "[ expr { $totalvirtualusers - 1 } ] Active Virtual Users configured"
        puts [ testresult $nopm $tpm $engine ]
        set errors {}
        if { "errors" in [ tsv::names ] } {
            foreach state [ lsort [ tsv::array names errors ] ] { lappend errors "$state=[ tsv::get errors $state ]" }
        }
        puts "Errors by SQLSTATE: [ expr { [ llength $errors ] ? [ join $errors { } ] : {none} } ]"
        tsv::set application abort 1
        catch { pg_disconnect $conn }
}

set rema [ lassign [ findvuposition ] myposition totalvirtualusers ]
switch $myposition {
    1 { monitor $host $port $user $db $rampup $duration $warehouses $totalvirtualusers $engine }
    default {
        set conn [ connect $host $port $user $db ]
        # A home warehouse fixed by position (docs/server_design.md): HammerDB's
        # random one draws correlated numbers across users.
        set w_id [ expr { ($myposition - 2) % $warehouses + 1 } ]
        set stock_level_d_id [ RandomNumber 1 10 ]
        set raise [ string is true -strict $RAISEERROR ]
        set n 0
        puts "Processing $total_iterations transactions with output suppressed..."
        for {set it 0} {$it < $total_iterations} {incr it} {
            if { [ tsv::get application abort ] } { break }
            set choice [ RandomNumber 1 23 ]
            set kind [ expr { $choice <= 10 ? "neword" : $choice <= 20 ? "payment" : $choice == 21 ? "delivery" : $choice == 22 ? "slev" : "ostat" } ]
            if { [ lsearch -exact $mix $kind ] < 0 } { continue }
            switch $kind {
                neword { transaction $conn $raise { neword $conn $w_id $warehouses } }
                payment {
                    # h_id: the harness's scheme, the position as the thread.
                    set hid [ expr { ($run << 48) | ($myposition << 40) | $n } ]
                    incr n
                    transaction $conn $raise { payment $conn $w_id $warehouses $hid }
                }
                delivery { transaction $conn $raise { delivery $conn $w_id } }
                slev { transaction $conn $raise { slev $conn $w_id $stock_level_d_id } }
                ostat { transaction $conn $raise { ostat $conn $w_id } }
            }
        }
        catch { pg_disconnect $conn }
    }
}
