HSPTOKEN_DATA( HSPTOKEN_UNDEFINED, "undefined", HSPTOKENTYPE_UNDEFINED, NULL)
HSPTOKEN_DATA( HSPTOKEN_STARTOBJ, "{", HSPTOKENTYPE_SYNTAX, NULL)
HSPTOKEN_DATA( HSPTOKEN_ENDOBJ, "}", HSPTOKENTYPE_SYNTAX, NULL)
HSPTOKEN_DATA( HSPTOKEN_SFLOW, "sFlow", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_SUBAGENTID, "subAgentId", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_COUNTERPOLLINGINTERVAL, "counterPollingInterval", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_PACKETSAMPLINGRATE, "packetSamplingRate", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_AGENTIP, "agentIP", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_COLLECTOR, "collector", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_CONTAINERD, "containerd", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_IP, "IP", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_UDPPORT, "UDPPort", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_UDP, "UDP", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_AGENT, "agent", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_UUID, "uuid", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_DBCONFIG, "dbconfig", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_DNSSD, "DNSSD", HSPTOKENTYPE_ATTRIB, "dns-sd {} [omit to turn off]")
HSPTOKEN_DATA( HSPTOKEN_DNS_SD, "dns-sd", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_DNSSD_DOMAIN, "DNSSD_domain", HSPTOKENTYPE_ATTRIB, "dns-sd { domain=.mycompany.com }")
HSPTOKEN_DATA( HSPTOKEN_DOMAIN, "domain", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_POLLING, "polling", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_SAMPLING, "sampling", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_HEADERBYTES, "headerBytes", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_ULOG, "ulog", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_ULOGGROUP, "ulogGroup", HSPTOKENTYPE_ATTRIB, "ulog { group=[n] }")
HSPTOKEN_DATA( HSPTOKEN_GROUP, "group", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_ULOGPROBABILITY, "ulogProbability", HSPTOKENTYPE_ATTRIB, "ulog { probability=[0.nn] }")
HSPTOKEN_DATA( HSPTOKEN_PROBABILITY, "probability", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_NFLOG, "nflog", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_NFLOGGROUP, "nflogGroup", HSPTOKENTYPE_ATTRIB, "nflog { group=[n] }")
HSPTOKEN_DATA( HSPTOKEN_NFLOGPROBABILITY, "nflogProbability", HSPTOKENTYPE_ATTRIB,  "nflog { probability=[0.nn] }")
HSPTOKEN_DATA( HSPTOKEN_PSAMPLE, "psample", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_LOOPBACK, "loopback", HSPTOKENTYPE_ATTRIB, "ignored")
HSPTOKEN_DATA( HSPTOKEN_JSON, "json", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_JSONPORT, "jsonPort", HSPTOKENTYPE_ATTRIB, "json { udpPort=[n] }")
HSPTOKEN_DATA( HSPTOKEN_JSONFIFO, "jsonFIFO", HSPTOKENTYPE_ATTRIB, "json { fifo=[path] }")
HSPTOKEN_DATA( HSPTOKEN_FIFO, "fifo", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_AGENTCIDR, "agent.cidr", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_DATAGRAMBYTES, "datagramBytes", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_REFRESH_ADAPTORS, "refreshAdaptors", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_CHECK_ADAPTORS, "checkAdaptors", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_REFRESH_VMS, "refreshVMs", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_SAMPLINGDIRECTION, "samplingDirection", HSPTOKENTYPE_ATTRIB, "psample { ingress=[on/off] egress=[on|off] }")
HSPTOKEN_DATA( HSPTOKEN_FORGET_VMS, "forgetVMs", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_PCAP, "pcap", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_DEV, "dev", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_SPEED, "speed", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_PROMISC, "promisc", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_VPORT, "vport", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_KVM, "kvm", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_XEN, "xen", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_XEN_UPDATE_DOMINFO, "xen.update.dominfo", HSPTOKENTYPE_ATTRIB, "xen { update.dominfo=[on|off] }")
HSPTOKEN_DATA( HSPTOKEN_UPDATE_DOMINFO, "update.dominfo", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_XEN_DSK, "xen.dsk", HSPTOKENTYPE_ATTRIB,  "xen { dsk=[path] }")
HSPTOKEN_DATA( HSPTOKEN_DSK, "dsk", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_VBD, "vbd", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_DOCKER, "docker", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_MODULES, "modules", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_CUMULUS, "cumulus", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_DENT, "dent", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_SWITCHPORT, "switchport", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_OVS, "OVS", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_NVML, "NVML", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_OS10, "OS10", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_OPX, "OPX", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_SONIC, "SONIC", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_TCP, "tcp", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_DBUS, "dbus", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_SYSTEMD, "systemd", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_DROP_PRIV, "dropPriv", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_EAPI, "eapi", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_PORT, "port", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_CGROUP_PROCS, "cgroup_procs", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_CGROUP_ACCT, "cgroup_acct", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_CGROUP_TRAFFIC, "markTraffic", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_NAMESPACE, "namespace", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_HOSTNAME, "hostname", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_DROPMON, "dropmon", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_START, "start", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_LIMIT, "limit", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_SW, "sw", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_HW, "hw", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_RN, "rn", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_HW_UNKNOWN, "hw_unknown", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_HW_FUNCTION, "hw_function", HSPTOKENTYPE_ATTRIB, "ignored")
HSPTOKEN_DATA( HSPTOKEN_TUNNEL, "tunnel", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_MAX, "max", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_UNIXSOCK, "unixsock", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_INGRESS, "ingress", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_EGRESS, "egress", HSPTOKENTYPE_ATTRIB, NULL)
HSPTOKEN_DATA( HSPTOKEN_K8S, "k8s", HSPTOKENTYPE_OBJ, NULL)
HSPTOKEN_DATA( HSPTOKEN_WAITREADY, "waitReady", HSPTOKENTYPE_ATTRIB, NULL)
