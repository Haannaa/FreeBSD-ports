<?php
/*
 * quagga_ospfd.inc
 *
 * part of pfSense (https://www.pfsense.org)
 * Copyright (c) 2010-2020 Rubicon Communications, LLC (Netgate)
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

require_once("config.inc");
require_once("functions.inc");
require_once("interfaces.inc");
require_once("service-utils.inc");
require_once("util.inc");

define('PKG_QUAGGA_CONFIG_BASE', '/var/etc/quagga');

function quagga_ospfd_get_carp_list() {

	$list = array();
	$list[] = array("name" => "none", "value" => "none");

	$carplist = get_configured_vip_list("all", VIP_CARP);
	foreach ($carplist as $vid => $vaddr) {
		$vip = get_configured_vip($vid);
		$list[] = array("name" => "$vaddr (vhid: {$vip['vhid']})", "value" => $vid);
	}

	return ($list);
}

function quagga_ospfd_get_interfaces() {
	global $config;
	$interfaces = get_configured_interface_with_descr();
	$ospf_ifs = array();
	foreach ($interfaces as $iface => $ifacename) {
		$tmp["name"] = $ifacename;
		$tmp["value"] = $iface;
		$ospf_ifs[] = $tmp;
	}

	foreach (array('server', 'client') as $mode) {
		if (is_array($config['openvpn']["openvpn-{$mode}"])) {
			foreach ($config['openvpn']["openvpn-{$mode}"] as $id => $setting) {
				if (!isset($setting['disable'])) {
					$tmp["name"] = gettext("OpenVPN") . " ".$mode.": ".htmlspecialchars($setting['description']);
					$tmp["value"] = 'ovpn' . substr($mode, 0, 1) . $setting['vpnid'];
					$ospf_ifs[] = $tmp;
				}
			}
		}
	}

	return $ospf_ifs;
}

function quagga_ospfd_install_conf() {
	global $config;

	// Since we need to embed this in a string, copy to a var. Can't embed constnats.
	$quagga_config_base = PKG_QUAGGA_CONFIG_BASE;

	$noaccept = "";

	// generate ospfd.conf based on the assistant
	if (is_array($config['installedpackages']['quaggaospfd']['config'])) {
		$ospfd_conf = &$config['installedpackages']['quaggaospfd']['config'][0];
	} elseif (isset($config['installedpackages']['quaggaospfdraw']['config'][0]['ospfd']) ||
	    isset($config['installedpackages']['quaggaospfdraw']['config'][0]['ospf6d']) ||
	    isset($config['installedpackages']['quaggaospfdraw']['config'][0]['bgpd'])) {
		log_error("Quagga: No assistant generated config for OSPF, but found raw config for one or more daemon");
	} else {
		log_error("Quagga OSPFd: No config data found.");
		return;
	}

	if (isset($config['installedpackages']['quaggaospfdraw']['config'][0]['ospfd'])
	    && !empty($config['installedpackages']['quaggaospfdraw']['config'][0]['ospfd'])) {
		// if there is a raw config specifyed in tthe config.xml use that instead of the assisted config
		$conffile = str_replace("\r","",base64_decode($config['installedpackages']['quaggaospfdraw']['config'][0]['ospfd']));
	} else {
		$conffile = "# This file was created by the pfSense package manager.  Do not edit!\n\n";

		if ($ospfd_conf['password']) {
			$conffile .= "password {$ospfd_conf['password']}\n";
		}
		if ($ospfd_conf['logging']) {
			$conffile .= "log syslog\n";
		}

		/* Interface Settings */
		$passive_interfaces = array();
		$interface_networks = array();

		if (is_array($config['installedpackages']['quaggaospfdinterfaces']['config'])) {
			foreach ($config['installedpackages']['quaggaospfdinterfaces']['config'] as $conf) {
				$realif = get_real_interface($conf['interface']);
				if (empty($realif)) {
					continue;
				}
				$conffile .= "interface {$realif}\n" ;
				if (!empty($conf['networktype'])) {
					$conffile .= "  ip ospf network {$conf['networktype']}\n";
				}
				if (!empty($conf['metric'])) {
					$conffile .= "  ip ospf cost {$conf['metric']}\n";
				}
				if (!empty($conf['hellointervalinseconds'])) {
					$conffile .= "  ip ospf hello-interval {$conf['hellointervalinseconds']}\n";
				}
				if ($conf['md5password'] && !empty($conf['password'])) {
					$conffile .= "  ip ospf authentication message-digest\n";
					$conffile .= "  ip ospf message-digest-key 1 md5 " . substr($conf['password'], 0, 16) . "\n";
				} elseif (!empty($conf['password'])) {
					$conffile .= "  ip ospf authentication-key " . substr($conf['password'], 0, 8) . "\n";
				}
				if (!empty($conf['routerpriorityelections']) || ($conf['routerpriorityelections'] == "0")) {
					$conffile .= "  ip ospf priority {$conf['routerpriorityelections']}\n";
				}
				if (!empty($conf['retransmitinterval'])) {
					$conffile .= "  ip ospf retransmit-interval {$conf['retransmitinterval']}\n";
				}
				if (!empty($conf['deadtimer'])) {
					$conffile .= "  ip ospf dead-interval {$conf['deadtimer']}\n";
				}
				if (!empty($conf['passive'])) {
					$passive_interfaces[] = $realif;
				}
				$interface_ip = find_interface_ip($realif);
				$interface_subnet = find_interface_subnet($realif);
				/* Cheap hack since point-to-points won't attach if /32 is used. */
				if ($interface_subnet == 32) {
					$interface_subnet = 30;
				}
				$subnet = gen_subnet($interface_ip, $interface_subnet);
				if (!empty($conf['acceptfilter']) && is_ipaddr($subnet) && is_numericint($interface_subnet)) {
					$noaccept .= "ip prefix-list ACCEPTFILTER deny {$subnet}/{$interface_subnet}\n";
				}
				if (!empty($conf['interfacearea'])) {
					$interface_networks[] = array( "subnet" => "{$subnet}/{$interface_subnet}", "area" => $conf['interfacearea']);
				} else {
					$interface_networks[] = array( "subnet" => "{$subnet}/{$interface_subnet}", "area" => $ospfd_conf['area']);
				}
 			}
		}


		$redist = "";
		$noredist = "";
		if (is_array($ospfd_conf['row'])) {
			foreach ($ospfd_conf['row'] as $redistr) {
				if (empty($redistr['routevalue'])) {
					continue;
				}
				if (isset($redistr['acceptfilter'])) {
					$noaccept .= "ip prefix-list ACCEPTFILTER deny {$redistr['routevalue']}\n";
				}
				if (isset($redistr['redistribute'])) {
					$noredist .= "  access-list dnr-list deny {$redistr['routevalue']}\n";
				} else {
					$area = ($redistr['routearea'] == "") ? $ospfd_conf['area'] : $redistr['routearea'];
					$redist .= "  network {$redistr['routevalue']} area {$area}\n";
				}
			}
		}

		/* OSPF Settings */

		$conffile .= "\n\nrouter ospf\n";

		// Specify router id
		if ($ospfd_conf['routerid']) {
			$conffile .= "  ospf router-id {$ospfd_conf['routerid']}\n";
		}
		/* In package versions <=0.6.15, this used to a checkbox handling stub areas only
		 * Leave the first check here
		 */
		if ($ospfd_conf['updatefib'] == "on" ) {
			$conffile .= "  area {$ospfd_conf['area']} stub\n";
		} elseif ($ospfd_conf['updatefib'] == "stub") {
			$conffile .= "  area {$ospfd_conf['area']} stub\n";
		} elseif ($ospfd_conf['updatefib'] == "nosum") {
			$conffile .= "  area {$ospfd_conf['area']} stub no-summary\n";
		} elseif ($ospfd_conf['updatefib'] == "nssa") {
			$conffile .= "  area {$ospfd_conf['area']} nssa\n";
		}
		if ($ospfd_conf['logging'] && $ospfd_conf['adjacencylog']) {
			$conffile .= "  log-adjacency-changes detail\n";
		}
		if ($ospfd_conf['redistributeconnectedsubnets']) {
			$conffile .= "  redistribute connected\n";
		}
		if ($ospfd_conf['redistributestatic']) {
			$conffile .= "  redistribute static\n";
		}
		if ($ospfd_conf['redistributekernel']) {
			$conffile .= "  redistribute kernel\n";
		}
		if ($ospfd_conf['redistributedefaultroute']) {
			$conffile .= "  default-information originate\n";
		}
		if ($ospfd_conf['spfholdtime'] || $ospfd_conf['spfdelay']) {
			$spf_minhold = ($ospfd_conf['spfholdtime']) ? $ospfd_conf['spfholdtime'] : 1000;
			$spf_maxhold = $spf_minhold * 10;
			$spf_delay = ($ospfd_conf['spfdelay']) ? $ospfd_conf['spfdelay'] : 200;
			$conffile .= "  timers throttle spf {$spf_delay} {$spf_minhold} {$spf_maxhold}\n";
		}
		if ($ospfd_conf['rfc1583']) {
			$conffile .= "  ospf rfc1583compatibility\n";
		}
		if (is_array($passive_interfaces)) {
			foreach ($passive_interfaces as $pint)
				$conffile .= "  passive-interface {$pint}\n";
		}
		if (is_array($interface_networks)) {
			foreach ($interface_networks as $ifn) {
				if (is_subnet($ifn['subnet'])) {
					$conffile .= "  network {$ifn['subnet']} area {$ifn['area']}\n";
				}
			}
		}
		if ($conf['md5password'] && !empty($conf['password']) && !empty($conf['interfacearea'])) {
			$conffile .= "  area {$conf['interfacearea']} authentication message-digest\n";
		} elseif ($conf['md5password'] && !empty($conf['password']) && empty($conf['interfacearea'])) {
			$conffile .= "  area 0.0.0.0 authentication message-digest\n";
		}
		if ($conf['password'] && empty($conf['md5password']) && !empty($conf['interfacearea'])) {
			$conffile .= "  area {$conf['interfacearea']} authentication\n";
		} elseif ($conf['password'] && empty($conf['md5password']) && empty($conf['interfacearea'])) {
			$conffile .= "  area 0.0.0.0 authentication\n";
		}
		if (!empty($redist)) {
			$conffile .= $redist;
		}
		if (!empty($noredist)) {
			$conffile .= "  distribute-list dnr-list out connected\n";
			$conffile .= "  distribute-list dnr-list out kernel\n";
			$conffile .= "  distribute-list dnr-list out static\n";
			//$conffile .= "  distribute-list dnr-list out ospf\n";
			$conffile .= $noredist;
			$conffile .= "  access-list dnr-list permit any\n";
		}

	}
	safe_mkdir($quagga_config_base);
	$fd = fopen("{$quagga_config_base}/ospfd.conf", "w");

	// Write out the configuration file
	fwrite($fd, $conffile);

	// Close file handle
	fclose($fd);

	/* Make zebra config */
	if (isset($config['installedpackages']['quaggaospfdraw']['config'][0]['zebra'])
	    && !empty($config['installedpackages']['quaggaospfdraw']['config'][0]['zebra'])) {
		// if there is a raw config specifyed in tthe config.xml use that instead of the assisted config
		$zebraconffile = str_replace("\r", "", base64_decode($config['installedpackages']['quaggaospfdraw']['config'][0]['zebra']));
	} else {
		$zebraconffile = "# This file was created by the pfSense package manager.  Do not edit!\n\n";
		if ($ospfd_conf['password']) {
			$zebraconffile .= "password {$ospfd_conf['password']}\n";
		}
		if ($ospfd_conf['logging']) {
			$zebraconffile .= "log syslog\n";
		}
		if (!empty($noaccept)) {
			$zebraconffile .= $noaccept;
			$zebraconffile .= "ip prefix-list ACCEPTFILTER permit any\n";
			$zebraconffile .= "route-map ACCEPTFILTER permit 10\n";
			$zebraconffile .= "match ip address prefix-list ACCEPTFILTER\n";
			$zebraconffile .= "ip protocol ospf route-map ACCEPTFILTER\n";
		}
	}
	$fd = fopen("{$quagga_config_base}/zebra.conf", "w");
	fwrite($fd, $zebraconffile);
	fclose($fd);

	/* Make bgpd config, add password and logging */
	if (isset($config['installedpackages']['quaggaospfdraw']['config'][0]['bgpd'])
	    && !empty($config['installedpackages']['quaggaospfdraw']['config'][0]['bgpd'])) {
		// if there is a raw config specified in the config.xml use that instead of the assisted config
		$bgpdconffile = str_replace("\r","",base64_decode($config['installedpackages']['quaggaospfdraw']['config'][0]['bgpd']));
	} else {
		$bgpdconffile = "# This file was created by the pfSense package manager.  Do not edit!\n\n";
		if ($ospfd_conf['password']) {
			$bgpdconffile .= "password {$ospfd_conf['password']}\n";
		}
		if ($ospfd_conf['logging']) {
			$bgpdconffile .= "log syslog\n";
		}
	}
	$fd = fopen("{$quagga_config_base}/bgpd.conf", "w");
	fwrite($fd, $bgpdconffile);
	fclose($fd);
	/* Clear existing md5 authentication sessions and remove existing md5 config files, if they exist. */
	mwexec("/bin/test -e {$quagga_config_base}/bgpddelmd5pw.conf && /sbin/setkey -f {$quagga_config_base}/bgpddelmd5pw.conf && /bin/rm -f {$quagga_config_base}/bgpddelmd5pw.conf");
	mwexec("/bin/test -e {$quagga_config_base}/bgpdaddmd5pw.conf && /bin/rm -f {$quagga_config_base}/bgpdaddmd5pw.conf");
	/* Builds BGPd md5 password configuration files based on confguration in pfSense config.xml. */
	if (isset($config['installedpackages']['quaggaospfdraw']['config'][0]['row']) && is_array($config['installedpackages']['quaggaospfdraw']['config'][0]['row']))
	{
		foreach ($config['installedpackages']['quaggaospfdraw']['config'][0]['row'] as $bgpdpw) {
			if (($bgpdpw['bgpdsourceaddr'] != "") && ($bgpdpw['bgpdpeeraddr'] != "") && ($bgpdpw['bgpdmd5pw'])!= "") {
				$bgpdaddmd5file .= "add {$bgpdpw['bgpdsourceaddr']} {$bgpdpw['bgpdpeeraddr']} tcp 0x1000 -A tcp-md5 \"{$bgpdpw['bgpdmd5pw']}\" ;\n";
				$bgpddelmd5file .= "delete {$bgpdpw['bgpdsourceaddr']} {$bgpdpw['bgpdpeeraddr']} tcp 0x1000 ;\n";
			}
		}
		if ($bgpdaddmd5file != "") {
			$fd = fopen("{$quagga_config_base}/bgpdaddmd5pw.conf", "w");
			fwrite($fd, $bgpdaddmd5file);
			fclose($fd);
		}
		if ($bgpddelmd5file != "") {
			$fd = fopen("{$quagga_config_base}/bgpddelmd5pw.conf", "w");
			fwrite($fd, $bgpddelmd5file);
			fclose($fd);
		}
	}
	/* Make ospf6 config */
	if (isset($config['installedpackages']['quaggaospfdraw']['config'][0]['ospf6d'])
	    && !empty($config['installedpackages']['quaggaospfdraw']['config'][0]['ospf6d'])) {
		// if there is a raw config specified in the config.xml use that instead of the assisted config
		$ospf6dconffile = str_replace("\r","",base64_decode($config['installedpackages']['quaggaospfdraw']['config'][0]['ospf6d']));
	} else {
		$ospf6dconffile = "";
	}

	$fd = fopen("{$quagga_config_base}/ospf6d.conf", "w");
	fwrite($fd, $ospf6dconffile);
	fclose($fd);

	//UPGRADE PATH - Checks if legacy "CARP Disable" mode was previously in use proior to 0.6.20; if previously in use, sets "carpmode" to "quaggadisable". If not in use, sets "carpmode" to "none".
	if (!isset($ospfd_conf['carpmode'])) {
		syslog(LOG_NOTICE, "Quagga: CARP failover mode upgrade initializing");
		if (strpos($ospfd_conf['carpstatusvid'], '_vip') !== false) {
			$ospfd_conf['carpmode'] = "quaggadisable";
			syslog(LOG_NOTICE, "Quagga: CARP failover mode established as 'quaggadisable'");
		} else {
			$ospfd_conf['carpmode'] = "none";
			syslog(LOG_NOTICE, "Quagga: CARP failover mode established as 'none'");
		}
		write_config( $desc = gettext("Quagga_OSPFd: Upgraded failover method for legacy configurations of Quagga_OSPFD") );
		syslog(LOG_NOTICE, "Quagga: CARP failover upgrade complete");
	}
	
	$carp_ip_status_check = "";
	if ((isset($ospfd_conf['carpmode']) && ($ospfd_conf['carpmode'] == "quaggadisable")) 
	    && (isset($ospfd_conf['carpstatusvid']) && ($ospfd_conf['carpstatusvid'] != "none"))) {
		$vip = get_configured_vip($ospfd_conf['carpstatusvid']);
		$carpcheckinterface = escapeshellarg(get_real_interface($vip['interface']));
		$vhid = escapeshellarg("vhid {$vip['vhid']}");
		$carp_ip_status_check = <<<EOF

CARP_STATUS=`/sbin/ifconfig {$carpcheckinterface} | /usr/bin/grep 'carp:' | /usr/bin/grep {$vhid} | /usr/bin/awk '{print \$2;}'`
if [ \${CARP_STATUS} != "MASTER" ]; then
	logger "Quagga: CARP \"Quagga Disable\" Mode - Interface {$carpcheckinterface} is NOT in MASTER state, exiting";
	exit;
fi
EOF;
	}
	
	$carp_ospfcost_status_check = "";
	if ((isset($ospfd_conf['carpmode']) && ($ospfd_conf['carpmode'] == "ospfcost"))
	    && (isset($ospfd_conf['carpcostvid']) && (substr($ospfd_conf['carpcostvid'], 0, 4) != "none"))
	    && (isset($ospfd_conf['carpactivecost']) && isset($ospfd_conf['carpbackupcost']))
	    && ($ospfd_conf['carpactivecost'] >= 0 && ($ospfd_conf['carpactivecost'] <= 65535))
	    && ($ospfd_conf['carpbackupcost'] >= 0 && ($ospfd_conf['carpbackupcost'] <= 65535))) {
		$control_script = "/usr/local/bin/quaggactl";
		$carpvips = explode(",", $ospfd_conf['carpcostvid']);
		foreach ($carpvips as $vips) {
		$vip = get_configured_vip($vips);
		$phyint = get_real_interface($vip['interface']);
		$carpcheckinterface = escapeshellarg(get_real_interface($vip['interface']));
		$vhid = escapeshellarg("vhid {$vip['vhid']}");
		$carp_ospfcost_status_check .= <<<EOF

CARP_STATUS=`/sbin/ifconfig {$carpcheckinterface} | /usr/bin/grep 'carp:' | /usr/bin/grep {$vhid} | /usr/bin/awk '{print \$2;}'`
if [ \${CARP_STATUS} == "MASTER" ]; then
	{$control_script} ospf cost {$phyint} {$ospfd_conf['carpactivecost']};
	logger "Quagga: CARP \"OSPF Cost\" Mode - Interface {$carpcheckinterface} is in MASTER state, OSPF cost set to {$ospfd_conf['carpactivecost']}";
else
	{$control_script} ospf cost {$phyint} {$ospfd_conf['carpbackupcost']};
	logger "Quagga: CARP \"OSPF Cost\" Mode - Interface {$carpcheckinterface} is NOT in MASTER state, OSPF cost set to {$ospfd_conf['carpbackupcost']}";
fi
EOF;
			
		}
	}


	// Create rc.d file
	$rc_file_stop = <<<EOF
if [ -e /var/run/quagga/zebra.pid ]; then
	# XXX: See Bug #6305 - Quagga 1.x does not like SIGKILL
	/bin/pkill -F /var/run/quagga/zebra.pid
	/bin/rm -f /var/run/quagga/zebra.pid
fi
if [ -e /var/run/quagga/ospfd.pid ]; then
	# XXX: See Bug #6305 - Quagga 1.x does not like SIGKILL
	/bin/pkill -F /var/run/quagga/ospfd.pid
	/bin/rm -f /var/run/quagga/ospfd.pid
fi
if [ -e /var/run/quagga/ospf6d.pid ]; then
	# XXX: See Bug #6305 - Quagga 1.x does not like SIGKILL
	/bin/pkill -F /var/run/quagga/ospf6d.pid
	/bin/rm -f /var/run/quagga/ospf6d.pid
fi
if [ -e /var/run/quagga/bgpd.pid ]; then
	# XXX: See Bug #6305 - Quagga 1.x does not like SIGKILL
	/bin/pkill -F /var/run/quagga/bgpd.pid
	/bin/rm -f /var/run/quagga/bgpd.pid
	/sbin/setkey -f {$quagga_config_base}/bgpddelmd5pw.conf
fi
EOF;
	$rc_file_start = <<<EOF
/bin/mkdir -p /var/run/quagga
/bin/mkdir -p /var/log/quagga
/bin/rm -f /var/run/quagga/zebra.pid
/bin/rm -f /var/run/quagga/ospfd.pid
/bin/rm -f /var/run/quagga/ospf6d.pid
/bin/rm -f /var/run/quagga/bgpd.pid

/usr/sbin/chown -R quagga:quagga {$quagga_config_base}
/usr/sbin/chown -R quagga:quagga /var/run/quagga
/usr/sbin/chown -R quagga:quagga /var/log/quagga
# Ensure no other copies of the daemons are running or it breaks.
sleep 1
if /bin/pgrep -q zebra; then
	/usr/bin/killall zebra
	sleep 1
	/usr/bin/killall -9 zebra 2>/dev/null
fi
if /bin/pgrep -q ospfd; then
	/usr/bin/killall ospfd
	sleep 1
	/usr/bin/killall -9 ospfd 2>/dev/null
fi
if /bin/pgrep -q ospf6d; then
	/usr/bin/killall ospf6d
	sleep 1
	/usr/bin/killall -9 ospf6d 2>/dev/null
fi
if /bin/pgrep -q bgpd; then
	/usr/bin/killall bgpd
	sleep 1
	/usr/bin/killall -9 bgpd 2>/dev/null
fi
{$carp_ip_status_check}
[ -s {$quagga_config_base}/zebra.conf ] && /usr/local/sbin/zebra -d -f {$quagga_config_base}/zebra.conf
[ -s {$quagga_config_base}/ospfd.conf ] && /usr/local/sbin/ospfd -d -f {$quagga_config_base}/ospfd.conf
[ -s {$quagga_config_base}/ospf6d.conf ] && /usr/local/sbin/ospf6d -d -f {$quagga_config_base}/ospf6d.conf
[ -s {$quagga_config_base}/bgpdaddmd5pw.conf ] && /sbin/setkey -f {$quagga_config_base}/bgpdaddmd5pw.conf
[ -s {$quagga_config_base}/bgpd.conf ] && /usr/local/sbin/bgpd -d -f {$quagga_config_base}/bgpd.conf
{$carp_ospfcost_status_check}

EOF;
	write_rcfile(array(
			"file" => "quagga.sh",
			"start" => $rc_file_start,
			"stop" =>  $rc_file_stop
		)
	);

	// Ensure files have correct permissions
	mwexec("/bin/chmod a+rx /usr/local/etc/rc.d/quagga.sh");
	mwexec("/bin/chmod u+rw,go-rw {$quagga_config_base}/ospfd.conf");
	mwexec("/bin/chmod u+rw,go-rw {$quagga_config_base}/ospf6d.conf");
	mwexec("/bin/chmod u+rw,go-rw {$quagga_config_base}/zebra.conf");
	mwexec("/bin/chmod u+rw,go-rw {$quagga_config_base}/bgpd.conf");

	// Kick off newly created rc.d script
	if ((isset($ospfd_conf['carpmode']) && ($ospfd_conf['carpmode'] == "quaggadisable")) &&
	    (isset($ospfd_conf['carpstatusvid']) && ($ospfd_conf['carpstatusvid'] != "none"))) {
		$status = get_carp_interface_status($ospfd_conf['carpstatusvid']);
		switch (strtoupper($status)) {
			// Stop the service if the VIP is in BACKUP or INIT state.
			case "BACKUP":
			case "INIT":
				mwexec_bg("/usr/local/etc/rc.d/quagga.sh stop");
				syslog(LOG_NOTICE, "Quagga: CARP \"Quagga Disable\" Mode - Interface {$carpcheckinterface} is NOT in MASTER state, exiting");
				break;
			// Start the service if the VIP is MASTER state.
			case "MASTER":
			// Assume it's up if the status can't be determined.
			default:
				syslog(LOG_NOTICE, "Quagga: CARP \"Quagga Disable\" Mode - Interface {$carpcheckinterface} is in MASTER state, starting");
				mwexec_bg("/usr/local/etc/rc.d/quagga.sh restart");
				break;
		}
	} else {
		mwexec_bg("/usr/local/etc/rc.d/quagga.sh restart");
	}
}

function quagga_ospfd_validate_interface() {
	global $config, $id, $input_errors;

	if ($config['installedpackages']['quaggaospfdinterfaces']['config']) {
		foreach ($config['installedpackages']['quaggaospfdinterfaces']['config'] as $index => $conf) {
			if ($index == 0) {
				continue;
			}
			if ($id != $index && $conf['interface'] == $_POST['interface']) {
				$input_errors[] = "Interface {$_POST['interface']} is already configured.";
			}
		}
	}
	if ($_POST['md5password'] && empty($_POST['password'])) {
		$input_errors[] = "Please input a password.";
	}
}

function quagga_ospfd_validate_input() {
	global $config, $input_errors;

	if ($_POST['password'] <> "" && (strpos($_POST['password'], "'") !== false)) {
		$input_errors[] = "Password cannot contain a single quote (')";
	}
	if (!empty($_POST['routerid']) && !is_ipaddr($_POST['routerid'])) {
		$input_errors[] = "Router ID must be an address.";
	}
	if (!is_ipaddr($_POST['area'])) {
		$input_errors[] = "Area needs to be a valid ip_address.";
	}
	if ($_POST['spfholdtime'] <> "" && ($_POST['spfholdtime'] < 1 || $_POST['spfholdtime'] > 5)) {
		$input_errors[] = "SPF holdtime needs to be between 1 and 5.";
	}
	if ($_POST['spfdelay'] <> "" && ($_POST['spfdelay'] < 1 || $_POST['spfdelay'] > 10)) {
		$input_errors[] = "SPF delay needs to be between 1 and 10.";
	}
	if (!$config['installedpackages']['quaggaospfdinterfaces']['config']) {
		$input_errors[] = "Please select an interface to use for Quagga OSPFd.";
	}
}

function quagga_ospfd_plugin_carp($pluginparams) {
	global $config;
	require_once("service-utils.inc");
	// Called when a CARP interface changes state
	// $pluginparams['event'] either 'rc.carpmaster' or 'rc.carpbackup'
	// $pluginparams['interface'] contains the affected interface

	/* If there is no OSPF config, then stop */
	if (is_array($config['installedpackages']['quaggaospfd']['config'])) {
		$ospfd_conf = &$config['installedpackages']['quaggaospfd']['config'][0];
	} else {
		return null;
	}
	/* If there is no properly configured CARP status check IP, then stop */
	if (!isset($ospfd_conf['carpmode']) || ($ospfd_conf['carpmode'] == "none")) {
		syslog(LOG_ALERT, "Quagga: CARP failover mode not set.");
		return null;
	}
	/* Verify "quaggadisable" parameters are properly set.*/
	if (($ospfd_conf['carpmode'] == "quaggadisable") && (!isset($ospfd_conf['carpstatusvid']) ||
	    ($ospfd_conf['carpstatusvid'] == "none"))) {
		syslog(LOG_ALERT, "Quagga: Failed checking CARP mode parameters: {$ospfd_conf['carpmode']}");
		return null;
	}
	/* Verify "carpmode" parameters are properly set.*/
	if (($ospfd_conf['carpmode'] == "ospfcost") && (!isset($ospfd_conf['carpcostvid']) ||
	    (substr($ospfd_conf['carpcostvid'], 0, 4) == "none")) ||
	    !isset($ospfd_conf['carpactivecost']) || !isset($ospfd_conf['carpbackupcost']) ||
	    (($ospfd_conf['carpactivecost'] < 0) || ($ospfd_conf['carpactivecost'] > 65535)) ||
	    (($ospfd_conf['carpbackupcost'] < 0) || ($ospfd_conf['carpbackupcost'] > 65535))) {
		syslog(LOG_ALERT, "Quagga: Failed checking CARP mode parameters: {$ospfd_conf['carpmode']}");
		return null;
	}

	list($vhid, $iface) = explode("@", trim($pluginparams['interface']));
	$friendly = convert_real_interface_to_friendly_interface_name($iface);

	switch ($ospfd_conf['carpmode']) {
		case "quaggadisable":
			syslog(LOG_INFO, "Quagga: CARP mode {$ospfd_conf['carpmode']}");
			$vip = get_configured_vip($ospfd_conf['carpstatusvid']);
			syslog(LOG_INFO, "Quagga: CARP VHID and Interface {$vip['vhid']},{$vip['interface']}");
			if (($vip['vhid'] != $vhid) || ($vip['interface'] != $friendly)) {
				return null;
			}

			/* Start or stop the service as needed based on the CARP transition. */
			if ((isset($ospfd_conf['carpmode']) && ($ospfd_conf['carpmode'] == "quaggadisable")) &&
			    (isset($ospfd_conf['carpstatusvid']) && ($ospfd_conf['carpstatusvid'] != "none"))) {
				if ($pluginparams['event'] == "rc.carpmaster") {
					start_service("Quagga OSPFd");
				} elseif ($pluginparams['event'] == "rc.carpbackup") {
					stop_service("Quagga OSPFd");
				}
			}
			break;
		case "ospfcost":
			syslog(LOG_INFO, "Quagga: CARP mode {$ospfd_conf['carpmode']}");
			$carpvips = explode(",", $ospfd_conf['carpcostvid']);
			$match = FALSE;
			$i = 0;
			foreach ($carpvips as $vip) {
				$vips[$i] = get_configured_vip($vip);
				if (($vips[$i]['vhid'] == $vhid) && ($vips[$i]['interface'] == $friendly)) {
					$match = TRUE;
					syslog(LOG_INFO, "Quagga: CARP VHID and Interface {$vips[$i]['vhid']} {$vips[$i]['interface']}");
					$phyint = get_real_interface($vips[$i]['interface']);
					syslog(LOG_INFO, "Quagga: CARP Interface identified {$phyint}");
				}
				$i++;
			}
			unset($vip);

			if ($match != TRUE){
				syslog(LOG_NOTICE, "Quagga: Couldn't find an Interface or VHID match!");
				return null;
			}

			/* Shift OSPF cost up or downas needed based on the CARP transition. */
			$control_script = "/usr/local/bin/quaggactl";
			if ((isset($ospfd_conf['carpmode']) && ($ospfd_conf['carpmode'] == "ospfcost")) &&
			    (isset($ospfd_conf['carpcostvid']) && (substr($ospfd_conf['carpcostvid'], 0, 4) != "none"))) {
				if ($pluginparams['event'] == "rc.carpmaster") {
					syslog(LOG_INFO, "Quagga: Shifted OSPF Master cost on {$phyint} to {$ospfd_conf['carpactivecost']}");
					mwexec("{$control_script} ospf cost {$phyint} {$ospfd_conf['carpactivecost']}");
					return null;
				} elseif ($pluginparams['event'] == "rc.carpbackup") {
					syslog(LOG_INFO, "Quagga: Shifted OSPF Backup cost on {$phyint} to {$ospfd_conf['carpbackupcost']}");
					mwexec("{$control_script} ospf cost {$phyint} {$ospfd_conf['carpbackupcost']}");
					return null;
				}
			}
			break;
		default:
			break;
	}
}

/* The following function checks for the presence of and writes the contents of "/var/etc/quagga/$module.conf" into the $module . "running" field within the config.xml file  */
function write_quagga_running_config($module) {
	global $config;
	// Since we need to embed this in a string, copy to a var. Can't embed constants.
	$quagga_config_base = PKG_QUAGGA_CONFIG_BASE;

	$moduleRunningFile = "{$quagga_config_base}/{$module}.conf";

	if ( file_exists( $moduleRunningFile ) && ( filesize( $moduleRunningFile ) > 0 ) ) {
		$moduleRunning = fopen( "$moduleRunningFile", "r" );
		$config['installedpackages']['quaggaospfdraw']['config'][0][$module . "running"] = base64_encode( fread( $moduleRunning, filesize($moduleRunningFile) ));
 	} else {
		$config['installedpackages']['quaggaospfdraw']['config'][0][$module . "running"] = base64_encode("!!!!! {$module}.conf does not exist or is empty.");
	}
	write_config( $desc = gettext("Quagga_OSPFd: Wrote {$module}.conf startup-config to pfSense config file."), $backup = false, $write_config_only = true );
}

write_quagga_running_config('bgpd');
write_quagga_running_config('ospfd');
write_quagga_running_config('ospf6d');
write_quagga_running_config('zebra');
?>
