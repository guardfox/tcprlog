# tcprlog
Live Remote Logging  Client/Server Application with buffering, get logs  even if session drops with reconnect support.

1. What it does

Fetchlogs is a tcp remote log program. Basically you have one
server where you want to have logs from other servers. There
are few options:
 - syslog udp logging
 - syslog tcp logging (central server have a syslog TCP in listen mode)
 - loggin to remote mysql/etc db server
 - periodic ftp/rsync/etc

First 3 solutions can loose messages. 4th will require some resources
(depends on how many servers etc). Fetchlogs try to avoid all
this issues. Logging on every server will be done to files (on disk) and
you will have to install a tcp server that will be used by fetchlogs
(which will run on central log server) to continuous download logs.
At this moment auth is per ip and no encryption is done

IF connection is lost..fetch logs tries to reconnect. Also it supports
logs rotation (for each log source there is a file where remote inode
is saved). Local log will be rotated when romote is
(that way you can simply run a md5 on remote and local to see if remote
was changed) 

So for each of this lines fetch_logs connects then wait for data. 
Server will send data (like tail -f do) or after 1min sends a
live signal (1 char with val 0x07). If fetch_logs lose connection or 
is idle for 2m it will reconect. Pointer file (/logs/remote_p/gogu.err.log)
will be updated when remote file rotates. In same time local file
will be rotated to /logs/remote/gogu.err.log.`date +%Y%m%d%H%M%S`

fetchlogs will run as a single proccess for all log sources(you 
can split the conf and run several fetchlogs if you dont like default)
gen_numbers will fork and create a child(standard unix behaviour)

2. Protocol
First client sends a line with  file inode offset. If it connects
for first time it sends only file.
Server replies also with a line S inode
S is status char (O for OK, L for lost)
Then server sends data from file starting at offset client provided
(0 if not specified). If requested inode is 0 or not set server opens 
the requested file (the ideea is if an inode is given server 
searchs the file* for that inode and sets L of not found)
Server will periodically check if file changed size and 
close connection if new_size < size (file was truncated) or
inode file changed (file rotated) or file is deleted. Else
will send the alive char


3. How to use
Server is done in perl ATM (gen_numbers.pl).Client (running
on central server) is fetch_logs (programmed in C using State Threads
lib)

fetch_logs config have lines like below one
1.2.3.4 19 /var/log/err.log /logs/remote/gogu.err.log /logs/remote_p/gogu.err.log

1.2.3.4 19 ip and client port
/var/log/err.log is the remote file
/logs/remote/gogu.err.log is local file where will put log lines
/logs/remote_p/gogu.err.log is pointer file which have remote inode

So how you can run it:
$ fetch_logs /var/snmp_data fetch_logs.conf 2>>fetch_logs.log >>fetch_logs.log2

to stderr fetch_logs will put fatal errors. to stdout you will see when
connection is lost, logs are rotated etc..

gen_numbers config file is called gen_numbers.pl.locals:

$hosts_allow{"127.0.0.1"} = 1;
$files{"/var/log/messages"} = "/var/log/messages.*[0-9]";
#$bind_port = 100;
#$bind_addr = "10.4.3.5";

/var/log/messages.*[0-9] will be looked if inode do not match
/var/log/messages (rotated ones)


fetch_logs will export status line in snmp_dir/$SERVER_IP and 

snmp_dir/$SERVER_IP is set to OK if there is no error with any
file under that server. snmp_from_file.pl
can be used with net-snmp by adding to snmpd.conf
pass_persist .1.3.6.1.4.99 _path_to_/snmp_from_file.pl
then snmpget .1.3.6.1.4.99.$IP and check for string OK

Tested on linux and freebsd

4. Copyleft
Source code is distributed under the terms of GNU General Public License (GPL) version 2
Also it includes State Threads lib ver 1.6

Authors: catam (cc^Hgg^Hm@route666.net)
