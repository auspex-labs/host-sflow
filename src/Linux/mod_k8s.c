/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <linux/types.h>
#include <sys/prctl.h>
#include <sched.h>
#include <openssl/sha.h>
#include <uuid/uuid.h>

#include "hsflowd.h"
#include "cpu_utils.h"
#include "math.h"

  // limit the number of chars we will read from each line
  // in /proc/net/dev and /prov/net/vlan/config
  // (there can be more than this - my_readline will chop for us)
#define MAX_PROC_LINE_CHARS 320

#include "cJSON.h"

  typedef struct _HSPK8sContainer {
    char uuid[16];
    char *id;
    char *name;
    char *hostname;
    pid_t pid;
    uint32_t state; // SFLVirDomainState
    uint64_t memoryLimit;
    uint32_t cpu_count;
    double cpu_count_dbl;
    uint64_t cpu_total;
    uint64_t mem_usage;
    SFLHost_nio_counters net;
    SFLHost_vrt_dsk_counters dsk;
  } HSPK8sContainer;

  typedef struct _HSPVMState_POD {
    HSPVMState vm; // superclass: must come first
    char *id;
    char *name;
    char *hostname;
    char *sandbox;
    pid_t pid;
    uint32_t state; // SFLVirDomainState
    uint32_t gpu_dev:1;
    uint32_t gpu_env:1;
    uint64_t memoryLimit;
    time_t last_vnic;
    time_t last_cgroup;
    char *cgroup_devices;
    // TODO: we now populate stats here too - or perhaps they can be rolled together
    // from the containers at the point where we report the counter samples?
    uint32_t cpu_count;
    double cpu_count_dbl;
    uint64_t cpu_total;
    uint64_t mem_usage;
    SFLHost_nio_counters net;
    SFLHost_vrt_dsk_counters dsk;
    UTHash *containers;
  } HSPVMState_POD;

#define HSP_CONTAINERD_READER "/usr/sbin/hsflowd_containerd"
#define HSP_CONTAINERD_DATAPREFIX "data>"

#define HSP_CONTAINERD_MAX_FNAME_LEN 255
#define HSP_CONTAINERD_MAX_LINELEN 512
#define HSP_CONTAINERD_SHORTID_LEN 12

#define HSP_CONTAINERD_WAIT_NOSOCKET 10
#define HSP_CONTAINERD_WAIT_EVENTDROP 5
#define HSP_CONTAINERD_WAIT_STARTUP 2
#define HSP_CONTAINERD_WAIT_RECHECK 120
#define HSP_CONTAINERD_WAIT_STATS 3
#define HSP_CONTAINERD_REQ_TIMEOUT 10

#define HSP_NVIDIA_VIS_DEV_ENV "NVIDIA_VISIBLE_DEVICES"
#define HSP_MAJOR_NVIDIA 195

#define MY_MAX_HOSTNAME_CHARS 255 // override sFlow standard of SFL_MAX_HOSTNAME_CHARS (64)

  typedef struct _HSPVNIC {
    SFLAddress ipAddr;
    uint32_t dsIndex;
    char *c_name;
    char *c_hostname;
    bool unique;
  } HSPVNIC;

#define HSP_VNIC_REFRESH_TIMEOUT 300
#define HSP_CGROUP_REFRESH_TIMEOUT 600

  typedef struct _HSP_mod_K8S {
    EVBus *pollBus;
    UTHash *vmsByID;
    UTHash *vmsBySandbox;
    UTHash *containersByID;
    SFLCounters_sample_element vnodeElem;
    int cgroupPathIdx;
    struct stat myNS;
    UTHash *vnicByIP;
    uint32_t configRevisionNo;
    pid_t readerPid;
  } HSP_mod_K8S;

  /*_________________---------------------------__________________
    _________________    utils to help debug    __________________
    -----------------___________________________------------------
  */

  char *containerStr(HSPK8sContainer *container, char *buf, int bufLen) {
    u_char uuidstr[100];
    printUUID((u_char *)container->uuid, uuidstr, 100);
    snprintf(buf, bufLen, "name: %s hostname: %s uuid: %s id: %s",
	     container->name,
	     container->hostname,
	     container->uuid,
	     container->id);
    return buf;
  }

  void containerHTPrint(UTHash *ht, char *prefix) {
    char buf[1024];
    HSPK8sContainer *container;
    UTHASH_WALK(ht, container)
      myLog(LOG_INFO, "%s: %s", prefix, containerStr(container, buf, 1024));
  }

  char *podStr(HSPVMState_POD *pod, char *buf, int bufLen) {
    u_char uuidstr[100];
    printUUID((u_char *)pod->vm.uuid, uuidstr, 100);
    snprintf(buf, bufLen, "name: %s hostname: %s uuid: %s id: %s",
	     pod->name,
	     pod->hostname,
	     pod->vm.uuid,
	     pod->id);
    return buf;
  }

  void podHTPrint(UTHash *ht, char *prefix) {
    char buf[1024];
    HSPVMState_POD *pod;
    UTHASH_WALK(ht, pod)
      myLog(LOG_INFO, "%s: %s", prefix, podStr(pod, buf, 1024));
  }

  /*________________---------------------------__________________
    ________________     podLinkCB             __________________
    ----------------___________________________------------------
    
    expecting lines of the form:
    VNIC: <ifindex> <device> <mac>
  */

  static int podLinkCB(EVMod *mod, HSPVMState_POD *pod, char *line) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    myDebug(1, "podLinkCB: line=<%s>", line);
    char deviceName[HSP_CONTAINERD_MAX_LINELEN];
    char macStr[HSP_CONTAINERD_MAX_LINELEN];
    char ipStr[HSP_CONTAINERD_MAX_LINELEN];
    uint32_t ifIndex;
    if(sscanf(line, "VNIC: %u %s %s %s", &ifIndex, deviceName, macStr, ipStr) == 4) {
      u_char mac[6];
      if(hexToBinary((u_char *)macStr, mac, 6) == 6) {
	SFLAdaptor *adaptor = adaptorListGet(pod->vm.interfaces, deviceName);
	if(adaptor == NULL) {
	  adaptor = nioAdaptorNew(deviceName, mac, ifIndex);
	  adaptorListAdd(pod->vm.interfaces, adaptor);
	  // add to "all namespaces" collections too - but only the ones where
	  // the id is really global.  For example,  many containers can have
	  // an "eth0" adaptor so we can't add it to sp->adaptorsByName.

	  // And because the containers are likely to be ephemeral, don't
	  // replace the global adaptor if it's already there.

	  if(UTHashGet(sp->adaptorsByMac, adaptor) == NULL)
	    if(UTHashAdd(sp->adaptorsByMac, adaptor) != NULL)
	      myDebug(1, "Warning: pod adaptor overwriting adaptorsByMac");

	  if(UTHashGet(sp->adaptorsByIndex, adaptor) == NULL)
	    if(UTHashAdd(sp->adaptorsByIndex, adaptor) != NULL)
	      myDebug(1, "Warning: pod adaptor overwriting adaptorsByIndex");

	  // mark it as a vm/pod device
	  // and record the dsIndex there for easy mapping later
	  // provided it is unique.  Otherwise set it to all-ones
	  // to indicate that it should not be used to map to pod.
	  HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);
	  nio->vm_or_container = YES;
	  if(nio->container_dsIndex != pod->vm.dsIndex) {
	    if(nio->container_dsIndex == 0)
	      nio->container_dsIndex = pod->vm.dsIndex;
	    else {
	      myDebug(1, "Warning: NIC already claimed by container with dsIndex==nio->container_dsIndex");
	      // mark is as not a unique mapping
	      nio->container_dsIndex = 0xFFFFFFFF;
	    }
	  }

	  // did we get an ip address too?
	  SFLAddress ipAddr = { };
	  if(parseNumericAddress(ipStr, NULL, &ipAddr, AF_INET)) {
	    if(!SFLAddress_isZero(&ipAddr)
	       && mdata->vnicByIP) {
	      myDebug(1, "VNIC: learned virtual ipAddr: %s", ipStr);
	      // Can use this to associate traffic with this pod
	      // if this address appears in sampled packet header as
	      // outer or inner IP
	      ADAPTOR_NIO(adaptor)->ipAddr = ipAddr;
	      HSPVNIC search = { .ipAddr = ipAddr };
	      HSPVNIC *vnic = UTHashGet(mdata->vnicByIP, &search);
	      if(vnic) {
		// found IP - check for non-unique mapping
		if(vnic->dsIndex != pod->vm.dsIndex) {
		  myDebug(1, "VNIC: IP %s clash between %s (ds=%u) and %s (ds=%u) -- setting unique=no",
			  ipStr,
			  vnic->c_hostname,
			  vnic->dsIndex,
			  pod->hostname,
			  pod->vm.dsIndex);
		  vnic->unique = NO;
		}
	      }
	      else {
		// add new VNIC entry
		vnic = (HSPVNIC *)my_calloc(sizeof(HSPVNIC));
		vnic->ipAddr = ipAddr;
		vnic->dsIndex = pod->vm.dsIndex;
		vnic->c_name = my_strdup(pod->name);
		vnic->c_hostname = my_strdup(pod->hostname);
		UTHashAdd(mdata->vnicByIP, vnic);
		vnic->unique = YES;
		myDebug(1, "VNIC: linked to %s (ds=%u)",
			vnic->c_hostname,
			vnic->dsIndex);
	      }
	    }
	  }

	}
      }
    }
    return YES;
  }

/*________________---------------------------__________________
  ________________      readPodInterfaces    __________________
  ----------------___________________________------------------
*/

#include <linux/version.h>
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,0,0) || (__GLIBC__ <= 2 && __GLIBC_MINOR__ < 14))
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000	/* New network namespace (lo, device, names sockets, etc) */
#endif

#define MY_SETNS(fd, nstype) syscall(__NR_setns, fd, nstype)
#else
#define MY_SETNS(fd, nstype) setns(fd, nstype)
#endif


  int readPodInterfaces(EVMod *mod, HSPVMState_POD *pod)  {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    pid_t nspid = pod->pid;
    myDebug(2, "readPodInterfaces: pid=%u", nspid);
    if(nspid == 0) return 0;

    // do the dirty work after a fork, so we can just exit afterwards,
    // same as they do in "ip netns exec"
    int pfd[2];
    if(pipe(pfd) == -1) {
      myLog(LOG_ERR, "pipe() failed : %s", strerror(errno));
      exit(EXIT_FAILURE);
    }
    pid_t cpid;
    if((cpid = fork()) == -1) {
      myLog(LOG_ERR, "fork() failed : %s", strerror(errno));
      exit(EXIT_FAILURE);
    }
    if(cpid == 0) {
      // in child
      close(pfd[0]);   // close read-end
      dup2(pfd[1], 1); // stdout -> write-end
      dup2(pfd[1], 2); // stderr -> write-end
      close(pfd[1]);

      // open /proc/<nspid>/ns/net
      char topath[HSP_CONTAINERD_MAX_FNAME_LEN+1];
      snprintf(topath, HSP_CONTAINERD_MAX_FNAME_LEN, PROCFS_STR "/%u/ns/net", nspid);
      int nsfd = open(topath, O_RDONLY | O_CLOEXEC);
      if(nsfd < 0) {
	fprintf(stderr, "cannot open %s : %s", topath, strerror(errno));
	exit(EXIT_FAILURE);
      }

      struct stat statBuf;
      if(fstat(nsfd, &statBuf) == 0) {
	myDebug(2, "pod namespace dev.inode == %u.%u", statBuf.st_dev, statBuf.st_ino);
	if(statBuf.st_dev == mdata->myNS.st_dev
	   && statBuf.st_ino == mdata->myNS.st_ino) {
	  myDebug(1, "skip my own namespace");
	  exit(0);
	}
      }

      /* set network namespace
	 CLONE_NEWNET means nsfd must refer to a network namespace
      */
      if(MY_SETNS(nsfd, CLONE_NEWNET) < 0) {
	fprintf(stderr, "seting network namespace failed: %s", strerror(errno));
	exit(EXIT_FAILURE);
      }

      /* From "man 2 unshare":  This flag has the same effect as the clone(2)
	 CLONE_NEWNS flag. Unshare the mount namespace, so that the calling
	 process has a private copy of its namespace which is not shared with
	 any other process. Specifying this flag automatically implies CLONE_FS
	 as well. Use of CLONE_NEWNS requires the CAP_SYS_ADMIN capability. */
      if(unshare(CLONE_NEWNS) < 0) {
	fprintf(stderr, "seting network namespace failed: %s", strerror(errno));
	exit(EXIT_FAILURE);
      }

      int fd = socket(PF_INET, SOCK_DGRAM, 0);
      if(fd < 0) {
	fprintf(stderr, "error opening socket: %d (%s)\n", errno, strerror(errno));
	exit(EXIT_FAILURE);
      }

      FILE *procFile = fopen(PROCFS_STR "/net/dev", "r");
      if(procFile) {
	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	char line[MAX_PROC_LINE_CHARS];
	int lineNo = 0;
	int truncated;
	while(my_readline(procFile, line, MAX_PROC_LINE_CHARS, &truncated) != EOF) {
	  if(lineNo++ < 2) continue; // skip headers
	  char buf[MAX_PROC_LINE_CHARS];
	  char *p = line;
	  char *devName = parseNextTok(&p, " \t:", NO, '\0', NO, buf, MAX_PROC_LINE_CHARS);
	  if(devName && my_strlen(devName) < IFNAMSIZ) {
	    strncpy(ifr.ifr_name, devName, sizeof(ifr.ifr_name)-1);
	    // Get the flags for this interface
	    if(ioctl(fd,SIOCGIFFLAGS, &ifr) < 0) {
	      fprintf(stderr, "pod device %s Get SIOCGIFFLAGS failed : %s",
		      devName,
		      strerror(errno));
	    }
	    else {
	      int up = (ifr.ifr_flags & IFF_UP) ? YES : NO;
	      int loopback = (ifr.ifr_flags & IFF_LOOPBACK) ? YES : NO;

	      if(up && !loopback) {
		// try to get ifIndex next, because we only care about
		// ifIndex and MAC when looking at pod interfaces
		if(ioctl(fd,SIOCGIFINDEX, &ifr) < 0) {
		  // only complain about this if we are debugging
		  myDebug(1, "pod device %s Get SIOCGIFINDEX failed : %s",
			  devName,
			  strerror(errno));
		}
		else {
		  int ifIndex = ifr.ifr_ifindex;
		  SFLAddress ipAddr = {};

		  // see if we can get an IP address
		  if(ioctl(fd,SIOCGIFADDR, &ifr) < 0) {
		    // only complain about this if we are debugging
		    myDebug(1, "device %s Get SIOCGIFADDR failed : %s",
			    devName,
			    strerror(errno));
		  }
		  else {
		    if (ifr.ifr_addr.sa_family == AF_INET) {
		      struct sockaddr_in *s = (struct sockaddr_in *)&ifr.ifr_addr;
		      // IP addr is now s->sin_addr
		      ipAddr.type = SFLADDRESSTYPE_IP_V4;
		      ipAddr.address.ip_v4.addr = s->sin_addr.s_addr;
		    }
		  }

		  // Get the MAC Address for this interface
		  if(ioctl(fd,SIOCGIFHWADDR, &ifr) < 0) {
		    myDebug(1, "device %s Get SIOCGIFHWADDR failed : %s",
			      devName,
			      strerror(errno));
		  }
		  else {
		    u_char macStr[13];
		    printHex((u_char *)&ifr.ifr_hwaddr.sa_data, 6, macStr, 12, NO);
		    char ipStr[64];
		    SFLAddress_print(&ipAddr, ipStr, 64);
		    // send this info back up the pipe to my my parent
		    printf("VNIC: %u %s %s %s\n", ifIndex, devName, macStr, ipStr);
		  }
		}
	      }
	    }
	  }
	}
      }

      // don't even bother to close file-descriptors,  just bail
      exit(0);

    }
    else {
      // in parent
      close(pfd[1]); // close write-end
      // read from read-end
      FILE *ovs;
      if((ovs = fdopen(pfd[0], "r")) == NULL) {
	myLog(LOG_ERR, "readPodInterfaces: fdopen() failed : %s", strerror(errno));
	return 0;
      }
      char line[MAX_PROC_LINE_CHARS];
      int truncated;
      while(my_readline(ovs, line, MAX_PROC_LINE_CHARS, &truncated) != EOF)
	podLinkCB(mod, pod, line);
      fclose(ovs);
      wait(NULL); // block here until child is done
    }

    return pod->vm.interfaces->num_adaptors;
  }

  /*________________-----------------------__________________
    ________________   getCounters_POD     __________________
    ----------------_______________________------------------
  */
  static void getCounters_POD(EVMod *mod, HSPVMState_POD *pod)
  {
    HSP *sp = (HSP *)EVROOTDATA(mod);
    SFL_COUNTERS_SAMPLE_TYPE cs = { 0 };
    HSPVMState *vm = (HSPVMState *)&pod->vm;

    if(sp->sFlowSettings == NULL) {
      // do nothing if we haven't settled on the config yet
      return;
    }

    // accumulate CPU, mem, diskI/O counters from containers
    // TODO: just accumulate into stack vars and remove these
    // fields from POD struct.
    pod->cpu_count = 0;
    pod->cpu_total = 0;
    pod->mem_usage = 0;
    pod->memoryLimit = 0;
    memset(&pod->dsk, 0, sizeof(pod->dsk));
    HSPK8sContainer *container;
    UTHASH_WALK(pod->containers, container) {
      pod->state = container->state;
      pod->cpu_count += container->cpu_count;
      pod->cpu_total += container->cpu_total;
      pod->mem_usage += container->mem_usage;
      pod->memoryLimit += container->memoryLimit;
      pod->dsk.capacity += container->dsk.capacity;
      pod->dsk.allocation += container->dsk.allocation;
      pod->dsk.available += container->dsk.available;
      pod->dsk.rd_req += container->dsk.rd_req;
      pod->dsk.rd_bytes += container->dsk.rd_bytes;
      pod->dsk.wr_req += container->dsk.wr_req;
      pod->dsk.wr_bytes += container->dsk.wr_bytes;
      pod->dsk.errs += container->dsk.errs;
      // TODO: accumulate net counters too?  (If they appear)
    }
    
    // host ID
    SFLCounters_sample_element hidElem = { 0 };
    hidElem.tag = SFLCOUNTERS_HOST_HID;
    char *hname = pod->hostname ?: pod->id; // TODO: consider config setting sp->containerd.hostname
    hidElem.counterBlock.host_hid.hostname.str = hname;
    hidElem.counterBlock.host_hid.hostname.len = my_strlen(hname);
    memcpy(hidElem.counterBlock.host_hid.uuid, vm->uuid, 16);

    // for pods we can show the same OS attributes as the parent
    hidElem.counterBlock.host_hid.machine_type = sp->machine_type;
    hidElem.counterBlock.host_hid.os_name = SFLOS_linux;
    hidElem.counterBlock.host_hid.os_release.str = sp->os_release;
    hidElem.counterBlock.host_hid.os_release.len = my_strlen(sp->os_release);
    SFLADD_ELEMENT(&cs, &hidElem);

    // host parent
    SFLCounters_sample_element parElem = { 0 };
    parElem.tag = SFLCOUNTERS_HOST_PAR;
    parElem.counterBlock.host_par.dsClass = SFL_DSCLASS_PHYSICAL_ENTITY;
    parElem.counterBlock.host_par.dsIndex = HSP_DEFAULT_PHYSICAL_DSINDEX;
    SFLADD_ELEMENT(&cs, &parElem);

    // VM Net I/O
    SFLCounters_sample_element nioElem = { 0 };
    nioElem.tag = SFLCOUNTERS_HOST_VRT_NIO;
    memcpy(&nioElem.counterBlock.host_vrt_nio, &pod->net, sizeof(pod->net));
    SFLADD_ELEMENT(&cs, &nioElem);

    // VM cpu counters [ref xenstat.c]
    SFLCounters_sample_element cpuElem = { 0 };
    cpuElem.tag = SFLCOUNTERS_HOST_VRT_CPU;
    cpuElem.counterBlock.host_vrt_cpu.state = pod->state;
    cpuElem.counterBlock.host_vrt_cpu.nrVirtCpu = pod->cpu_count ?: (uint32_t)round(pod->cpu_count_dbl);
    cpuElem.counterBlock.host_vrt_cpu.cpuTime = (uint32_t)(pod->cpu_total / 1000000); // convert to mS
    SFLADD_ELEMENT(&cs, &cpuElem);

    SFLCounters_sample_element memElem = { 0 };
    memElem.tag = SFLCOUNTERS_HOST_VRT_MEM;
    memElem.counterBlock.host_vrt_mem.memory = pod->mem_usage;
    memElem.counterBlock.host_vrt_mem.maxMemory = pod->memoryLimit;
    SFLADD_ELEMENT(&cs, &memElem);

    // VM disk I/O counters
    SFLCounters_sample_element dskElem = { 0 };
    dskElem.tag = SFLCOUNTERS_HOST_VRT_DSK;
    // TODO: fill in capacity, allocation, available fields
    memcpy(&dskElem.counterBlock.host_vrt_dsk, &pod->dsk, sizeof(pod->dsk));
    SFLADD_ELEMENT(&cs, &dskElem);

    // include my slice of the adaptor list (the ones from my private namespace)
    SFLCounters_sample_element adaptorsElem = { 0 };
    adaptorsElem.tag = SFLCOUNTERS_ADAPTORS;
    adaptorsElem.counterBlock.adaptors = vm->interfaces;
    SFLADD_ELEMENT(&cs, &adaptorsElem);

    // circulate the cs to be annotated by other modules before it is sent out.
    HSPPendingCSample ps = { .poller = vm->poller, .cs = &cs };
    EVEvent *evt_vm_cs = EVGetEvent(sp->pollBus, HSPEVENT_VM_COUNTER_SAMPLE);
    // TODO: can we specify pollBus only? Receiving this on another bus would
    // be a disaster as we would not copy the whole structure here.
    EVEventTx(sp->rootModule, evt_vm_cs, &ps, sizeof(ps));
    if(ps.suppress) {
      sp->telemetry[HSP_TELEMETRY_COUNTER_SAMPLES_SUPPRESSED]++;
    }
    else {
      SEMLOCK_DO(sp->sync_agent) {
	sfl_poller_writeCountersSample(vm->poller, &cs);
	sp->counterSampleQueued = YES;
	sp->telemetry[HSP_TELEMETRY_COUNTER_SAMPLES]++;
      }
    }
  }

  static void agentCB_getCounters_POD_request(void *magic, SFLPoller *poller, SFL_COUNTERS_SAMPLE_TYPE *cs)
  {
  }

  /*_________________---------------------------__________________
    _________________    name_uuid              __________________
    -----------------___________________________------------------
    TODO: decide how to share this with mod_systemd.  Requires link with -lcrypto,
    and include <openssl/sha.h>
  */
#if 0
  static void uuidgen_type5(HSP *sp, u_char *uuid, char *name) {
    // Generate type 5 UUID (rfc 4122)
    SHA_CTX ctx;
    unsigned char sha_bits[SHA_DIGEST_LENGTH];
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, sp->uuid, 16); // use sp->uuid as "namespace UUID"
    SHA1_Update(&ctx, name, my_strlen(name));
    // also hash in agent IP address in case sp->uuid is missing or not unique
    SHA1_Update(&ctx,
		&sp->agentIP.address,
		(sp->agentIP.type == SFLADDRESSTYPE_IP_V6 ? 16 : 4));
    SHA1_Final(sha_bits, &ctx);
    // now generate a type-5 UUID according to the recipe here:
    // http://stackoverflow.com/questions/10867405/generating-v5-uuid-what-is-name-and-namespace
    // SHA1 Digest:   74738ff5 5367 e958 9aee 98fffdcd1876 94028007
    // UUID (v5):     74738ff5-5367-5958-9aee-98fffdcd1876
    //                          ^_low nibble is set to 5 to indicate type 5
    //                                   ^_first two bits set to 1 and 0, respectively
    memcpy(uuid, sha_bits, 16);
    uuid[6] &= 0x0F;
    uuid[6] |= 0x50;
    uuid[8] &= 0x3F;
    uuid[8] |= 0x80;
  }
#endif
  
  static void uuidgen_type5(HSP *sp, u_char *uuid, char *name) {
    int len = my_strlen(name);
    // also hash in agent IP address in case sp->uuid is missing or not unique
    int addrLen = sp->agentIP.type == SFLADDRESSTYPE_IP_V6 ? 16 : 4;
    char *buf = (char *)UTHeapQNew(len + addrLen);
    memcpy(buf, name, len);
    memcpy(buf + len, &sp->agentIP.address, addrLen);
    uuid_generate_sha1(uuid, (u_char *)sp->uuid, buf, len + addrLen);
  }

  /*_________________---------------------------__________________
    _________________   add and remove VM       __________________
    -----------------___________________________------------------
  */

  static void removePodVNICLookup(EVMod *mod, HSPVMState_POD *pod) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    SFLAdaptor *ad;
    ADAPTORLIST_WALK(pod->vm.interfaces, ad) {
      HSPAdaptorNIO *nio = ADAPTOR_NIO(ad);
      if(nio->ipAddr.type != SFLADDRESSTYPE_UNDEFINED) {
	HSPVNIC search = { };
	search.ipAddr = nio->ipAddr;
	HSPVNIC *vnic = UTHashDelKey(mdata->vnicByIP, &search);
	if(vnic) {
	  my_free(vnic->c_name);
	  my_free(vnic->c_hostname);
	  my_free(vnic);
	}
      }
    }
  }

  static void removeAndFreeContainer(EVMod *mod, HSPK8sContainer *container) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    if(getDebug()) {
      myLog(LOG_INFO, "removeAndFreeContainer: removing container with name=%s", container->name);
    }

    // remove from hash table
    if(UTHashDel(mdata->containersByID, container) == NULL) {
      myLog(LOG_ERR, "UTHashDel (containerssByID) failed: container %s=%s", container->name, container->id);
      if(debug(1))
	containerHTPrint(mdata->containersByID, "containersByID");
    }

    if(container->id)
      my_free(container->id);
    if(container->name)
      my_free(container->name);
    if(container->hostname)
      my_free(container->hostname);
    
    my_free(container);
  }

  static void removeAndFreeVM_POD(EVMod *mod, HSPVMState_POD *pod) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    if(getDebug()) {
      myLog(LOG_INFO, "removeAndFreeVM: removing pod with dsIndex=%u", pod->vm.dsIndex);
    }

    // remove any VNIC lookups by IP (this semaphore-protected hash table is point
    // of contact between poll thread and packet thread).
    // (the interfaces will be removed completely in removeAndFreeVM() below)
    if(mdata->vnicByIP)
      removePodVNICLookup(mod, pod);
    
    HSPK8sContainer *container;
    UTHASH_WALK(pod->containers, container)
      removeAndFreeContainer(mod, container);

    UTHashFree(pod->containers);

    // remove from hash tables
    if(UTHashDel(mdata->vmsByID, pod) == NULL) {
      myLog(LOG_ERR, "UTHashDel (vmsByID) failed: pod %s=%s", pod->name, pod->id);
      if(debug(1))
	podHTPrint(mdata->vmsByID, "vmsByID");
    }
    if(UTHashDel(mdata->vmsBySandbox, pod) == NULL) {
      myLog(LOG_ERR, "UTHashDel (vmsBySandbox) failed: pod %s=%s", pod->name, pod->id);
      if(debug(1))
	podHTPrint(mdata->vmsBySandbox, "vmsBySandbox");
    }

    if(pod->id)
      my_free(pod->id);
    if(pod->name)
      my_free(pod->name);
    if(pod->hostname)
      my_free(pod->hostname);

    removeAndFreeVM(mod, &pod->vm);
  }

  static HSPVMState_POD *getPod(EVMod *mod, char *id, char *sandbox, bool create) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(id == NULL) return NULL;
    HSPVMState_POD cont = { .id = id };
    HSPVMState_POD *pod = UTHashGet(mdata->vmsByID, &cont);
    if(pod == NULL
       && create) {
      char uuid[16];
      // turn pod ID into a UUID - just take the first 16 bytes of the id
      if(parseUUID(id, uuid) == NO) {
	myLog(LOG_ERR, "parsing pod UUID from <%s> - fall back on auto-generated", id);
	uuidgen_type5(sp, (u_char *)uuid, id);
      }

      pod = (HSPVMState_POD *)getVM(mod, uuid, YES, sizeof(HSPVMState_POD), VMTYPE_POD, agentCB_getCounters_POD_request);
      assert(pod != NULL);
      if(pod) {
	pod->id = my_strdup(id);
	pod->sandbox = my_strdup(sandbox);
	// add to collections
	UTHashAdd(mdata->vmsByID, pod);
	UTHashAdd(mdata->vmsBySandbox, pod);
	// collection of child containers
	pod->containers = UTHASH_NEW(HSPK8sContainer, id, UTHASH_SKEY);
      }
    }
    return pod;
  }
  
  static bool podDone(EVMod *mod, HSPVMState_POD *pod) {
    return (pod
	    && pod->state != SFL_VIR_DOMAIN_RUNNING);
  }

  /*_________________---------------------------__________________
    _________________  add and remove container __________________
    -----------------___________________________------------------
  */

  static HSPVMState_POD *getPodBySandbox(EVMod *mod, char *sandbox) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    if(sandbox == NULL)
      return NULL;
    HSPVMState_POD search = { .sandbox = sandbox };
    return UTHashGet(mdata->vmsBySandbox, &search);
  }

  static HSPVMState_POD *podAddContainer(EVMod *mod, HSPVMState_POD *pod, HSPK8sContainer *container) {
    return UTHashGetOrAdd(pod->containers, container);
  }

  static HSPK8sContainer *getContainer(EVMod *mod, char *id, bool create) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(id == NULL) return NULL;
    HSPK8sContainer cont = { .id = id };
    HSPK8sContainer *container = UTHashGet(mdata->containersByID, &cont);
    if(container == NULL
       && create) {
      char uuid[16];
      // turn container ID into a UUID - just take the first 16 bytes of the id
      if(parseUUID(id, uuid) == NO) {
	myLog(LOG_ERR, "parsing container UUID from <%s> - fall back on auto-generated", id);
	uuidgen_type5(sp, (u_char *)uuid, id);
      }

      container = (HSPK8sContainer *)UTHeapQNew(sizeof(HSPK8sContainer));
      memcpy(container->uuid, uuid, 16);
      container->id = my_strdup(id);
      // add to collection
      UTHashAdd(mdata->containersByID, container);
    }
    return container;
  }

  /*_________________---------------------------__________________
    _________________  updatePodAdaptors        __________________
    -----------------___________________________------------------
  */

  static void updatePodAdaptors(EVMod *mod, HSPVMState_POD *pod) {
    HSP *sp = (HSP *)EVROOTDATA(mod);
    HSPVMState *vm = &pod->vm;
    if(vm) {
      // reset the information that we are about to refresh
      adaptorListMarkAll(vm->interfaces);
      // then refresh it
      readPodInterfaces(mod, pod);
      // and clean up
      deleteMarkedAdaptors_adaptorList(sp, vm->interfaces);
      adaptorListFreeMarked(vm->interfaces);
    }
  }

  /*_________________-----------------------------__________________
    _________________  updateContainerCgroupPaths __________________
    -----------------_____________________________------------------
  */

  static void updatePodCgroupPaths(EVMod *mod, HSPVMState_POD *container) {
    HSPVMState *vm = &container->vm;
    if(vm) {
      // open /proc/<pid>/cgroup
      char cgpath[HSP_CONTAINERD_MAX_FNAME_LEN+1];
      snprintf(cgpath, HSP_CONTAINERD_MAX_FNAME_LEN, PROCFS_STR "/%u/cgroup", container->pid);
      FILE *procFile = fopen(cgpath, "r");
      if(procFile) {
	char line[MAX_PROC_LINE_CHARS];
	int truncated;
	while(my_readline(procFile, line, MAX_PROC_LINE_CHARS, &truncated) != EOF) {
	  if(!truncated) {
	    // expect lines like 3:devices:<long_path>
	    int entryNo;
	    char type[MAX_PROC_LINE_CHARS];
	    char path[MAX_PROC_LINE_CHARS];
	    if(sscanf(line, "%d:%[^:]:%[^:]", &entryNo, type, path) == 3) {
	      if(my_strequal(type, "devices")) {
		if(!my_strequal(container->cgroup_devices, path)) {
		  if(container->cgroup_devices)
		    my_free(container->cgroup_devices);
		  container->cgroup_devices = my_strdup(path);
		  myDebug(1, "containerd: container(%s)->cgroup_devices=%s", container->name, container->cgroup_devices);
		}
	      }
	    }
	  }
	}
	fclose(procFile);
      }
    }
  }

  /*_________________---------------------------__________________
    _________________   buildRegexPatterns      __________________
    -----------------___________________________------------------
  */
  static void buildRegexPatterns(EVMod *mod) {
    // HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
  }

  /*_________________---------------------------__________________
    _________________   host counter sample     __________________
    -----------------___________________________------------------
  */

  static void evt_host_cs(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    SFL_COUNTERS_SAMPLE_TYPE *cs = *(SFL_COUNTERS_SAMPLE_TYPE **)data;
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    if(!hasVNodeRole(mod, HSP_VNODE_PRIORITY_CONTAINERD))
      return;

    memset(&mdata->vnodeElem, 0, sizeof(mdata->vnodeElem));
    mdata->vnodeElem.tag = SFLCOUNTERS_HOST_VRT_NODE;
    mdata->vnodeElem.counterBlock.host_vrt_node.mhz = sp->cpu_mhz;
    mdata->vnodeElem.counterBlock.host_vrt_node.cpus = sp->cpu_cores;
    mdata->vnodeElem.counterBlock.host_vrt_node.num_domains = UTHashN(mdata->vmsByID);
    mdata->vnodeElem.counterBlock.host_vrt_node.memory = sp->mem_total;
    mdata->vnodeElem.counterBlock.host_vrt_node.memory_free = sp->mem_free;
    SFLADD_ELEMENT(cs, &mdata->vnodeElem);
  }

  /*_________________---------------------------__________________
    _________________     container names       __________________
    -----------------___________________________------------------
  */

  static void setContainerName(EVMod *mod, HSPK8sContainer *container, const char *name) {
    char *str = (char *)name;
    if(str && str[0] == '/') str++; // consume leading '/'
    if(my_strequal(str, container->name) == NO) {
      if(container->name)
	my_free(container->name);
      container->name = my_strdup(str);
    }
  }
  
  static void setContainerHostname(EVMod *mod, HSPK8sContainer *container, const char *hostname) {
    if(my_strequal(hostname, container->hostname) == NO) {
      if(container->hostname)
	my_free(container->hostname);
      myDebug(1, "setContainerHostname assigning hostname=%s", hostname);
      container->hostname = my_strdup(hostname);
    }
  }

  /*_________________---------------------------__________________
    _________________           GPUs            __________________
    -----------------___________________________------------------
  */

  static void clearPodGPUs(EVMod *mod, HSPVMState_POD *pod) {
    // clear out the list - we are single threaded on the
    // poll bus so there is no need for sync
    UTArray *arr = pod->vm.gpus;
    HSPGpuID *entry;
    UTARRAY_WALK(arr, entry)
      my_free(entry);
    UTArrayReset(arr);
  }

  static void readPodGPUsFromEnv(EVMod *mod, HSPVMState_POD *pod, cJSON *jenv) {
    // look through env vars for evidence of GPUs assigned to this pod
    int entries = cJSON_GetArraySize(jenv);
    UTArray *arr = pod->vm.gpus;
    for(int ii = 0; ii < entries; ii++) {
      cJSON *varval = cJSON_GetArrayItem(jenv, ii);
      if(varval) {
	char *vvstr = varval->valuestring;
	int vlen = strlen(HSP_NVIDIA_VIS_DEV_ENV);
	if(vvstr
	   && my_strnequal(vvstr, HSP_NVIDIA_VIS_DEV_ENV, vlen)
	   && vvstr[vlen] == '=') {
	  myDebug(2, "parsing GPU env: %s", vvstr);
	  char *gpu_uuids = vvstr + vlen + 1;
	  clearPodGPUs(mod, pod);
	  // (re)populate
	  char *str;
	  char buf[128];
	  while((str = parseNextTok(&gpu_uuids, ",", NO, 0, YES, buf, 128)) != NULL) {
	    myDebug(2, "parsing GPU uuidstr: %s", str);
	    // expect GPU-<uuid>
	    if(my_strnequal(str, "GPU-", 4)) {
	      HSPGpuID *gpu = my_calloc(sizeof(HSPGpuID));
	      if(parseUUID(str + 4, gpu->uuid)) {
		gpu->has_uuid = YES;
		myDebug(2, "adding GPU uuid to pod: %s", pod->name);
		UTArrayAdd(arr, gpu);
		pod->gpu_env = YES;
	      }
	      else {
		myDebug(2, "GPU uuid parse failed");
		my_free(gpu);
	      }
	    }
	  }
	}
      }
    }
  }
  

  static void readPodGPUsFromDev(EVMod *mod, HSPVMState_POD *pod) {
    // look through devices to see if individial GPUs are exposed
    char path[HSP_MAX_PATHLEN];
    sprintf(path, SYSFS_STR "/fs/cgroup/devices/%s/devices.list", pod->cgroup_devices);
    FILE *procFile = fopen(path, "r");
    if(procFile) {
      UTArray *arr = pod->vm.gpus;

      // if we already know this is our source of truth
      // for GPUs then clear the array now
      if(pod->gpu_dev)
	clearPodGPUs(mod, pod);

      char line[MAX_PROC_LINE_CHARS];
      int truncated;
      while(my_readline(procFile, line, MAX_PROC_LINE_CHARS, &truncated) != EOF) {
	if(!truncated) {
	  // expect lines like "c 195:1 rwm"
	  // Note that if we don't have broad capabilities we
	  // will only see "a *:* rwm" here and it won't mean anything. For
	  // example, if hsflowd is running as a container/pod it will probably
	  // need to be invoked with privileged:true for this to work.
	  // TODO: figure out what capabilities are actually required.
	  char chr_blk;
	  int major,minor;
	  char permissions[MAX_PROC_LINE_CHARS];
	  if(sscanf(line, "%c %d:%d %s", &chr_blk, &major, &minor, permissions) == 4) {
	    if(major == HSP_MAJOR_NVIDIA
	       && minor < 255) {

	      if(!pod->gpu_dev) {
		// Found one, so this is going to work. Establish
		// this as our source of truth for GPUs and clear
		// out any that might have been found another way.
		pod->gpu_dev = YES;
		clearPodGPUs(mod, pod);
	      }
	      HSPGpuID *gpu = my_calloc(sizeof(HSPGpuID));
	      gpu->minor = minor;
	      gpu->has_minor = YES;
	      myDebug(2, "adding GPU dev to pod: %s", pod->name);
	      UTArrayAdd(arr, gpu);
	    }
	  }
	}
      }
      fclose(procFile);
    }
  }

  /*_________________---------------------------__________________
    _________________       logField            __________________
    -----------------___________________________------------------
  */

  static void logField(int debugLevel, char *msg, cJSON *obj, char *field)
  {
    if(debug(debugLevel)) {
      cJSON *fieldObj = cJSON_GetObjectItem(obj, field);
      char *str = fieldObj ? cJSON_Print(fieldObj) : NULL;
      myLog(LOG_INFO, "%s %s=%s", msg, field, str ?: "<not found>");
      if(str)
	my_free(str);
    }
  }

  /*_________________---------------------------__________________
    _________________     readContainerJSON     __________________
    -----------------___________________________------------------
  */

  static void readContainerJSON(EVMod *mod, cJSON *top, void *magic) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    if(sp->sFlowSettings == NULL) {
      // do nothing if we haven't settled on the config yet
      return;
    }
    HSPK8sContainer *container = NULL;
    cJSON *jid = cJSON_GetObjectItem(top, "Id");

    if(jid)
      container = getContainer(mod, jid->valuestring, YES);
    if(container == NULL)
      return;

    cJSON *jpid = cJSON_GetObjectItem(top, "Pid");
    if(jpid)
      container->pid = (pid_t)jpid->valueint;
    
    cJSON *jmetrics = cJSON_GetObjectItem(top, "Metrics");
    if(!jmetrics)
      return;

    bool isSandbox = NO;
    cJSON *jnames = cJSON_GetObjectItem(jmetrics, "Names");
    if(jnames) {
      cJSON *jcgpth = cJSON_GetObjectItem(jnames, "CgroupsPath");
      if(jcgpth) {
	myDebug(1, "cgroupspath=%s\n", jcgpth->valuestring);
      }
      logField(1, " ", jnames, "Image");
      logField(1, " ", jnames, "Hostname");
      logField(1, " ", jnames, "ContainerName");
      logField(1, " ", jnames, "ContainerType");
      logField(1, " ", jnames, "SandboxName");
      logField(1, " ", jnames, "SandboxNamespace");
      logField(1, " ", jnames, "ImageName");
    }
    
    setContainerName(mod, container, jid->valuestring);
    
    cJSON *jn = cJSON_GetObjectItem(jnames, "ContainerName");
    cJSON *jt = cJSON_GetObjectItem(jnames, "ContainerType");
    cJSON *jhn = cJSON_GetObjectItem(jnames, "Hostname");
    cJSON *jsn = cJSON_GetObjectItem(jnames, "SandboxName");
    cJSON *jsns = cJSON_GetObjectItem(jnames, "SandboxNamespace");
    char *jn_s = (jn && strlen(jn->valuestring)) ? jn->valuestring : NULL;
    char *jt_s = (jt && strlen(jt->valuestring)) ? jt->valuestring : NULL;
    char *jhn_s = (jhn && strlen(jhn->valuestring)) ? jhn->valuestring : NULL;
    char *jsn_s = (jsn && strlen(jsn->valuestring)) ? jsn->valuestring : NULL;
    char *jsns_s = (jsns && strlen(jsns->valuestring)) ? jsns->valuestring : NULL;

    // remember if containerType is sandbox
    if(my_strequal(jt_s, "sandbox"))
      isSandbox = YES;

    // build the sandbox name - hopefully one that is unique to
    // this node and that every container in the pod  will agree on
    char compoundSandbox[MY_MAX_HOSTNAME_CHARS+1];
    snprintf(compoundSandbox, MY_MAX_HOSTNAME_CHARS, "%s_%s",
	     jsns_s ?: "",
	     jsn_s);

    // set hostname
    // From kubernetes/pgk/kubelet/dockershim/naming.go
    // Sandbox
    // k8s_POD_{s.name}_{s.namespace}_{s.uid}_{s.attempt}
    // Container
    // k8s_{c.name}_{s.name}_{s.namespace}_{s.uid}_{c.attempt}
    
    // Match the Kubernetes docker_inspect output by combining these strings into
    // the form k8s_<containername>_<sandboxname>_<sandboxnamespace>_<sandboxuser>_<c.attempt>
    // pull out name, hostname, sandboxname and sandboxnamespace
    // container name can be empty, so if it ends up being the
    // same as the sandbox name or hostname then we leave it out to save space (and to
    // prevent the combination of namespace.containername from exploding unexpectedly)
    if(my_strequal(jn_s, jsn_s))
      jn_s = NULL;
    if(my_strequal(jn_s, jhn_s))
      jn_s = NULL;
    // assemble,  with fake 'uid' and 'attempt' fields since we don't know them,
    // but trying not to use up all the quota for the sFlow string.
    char compoundName[MY_MAX_HOSTNAME_CHARS+1];
    snprintf(compoundName, MY_MAX_HOSTNAME_CHARS, "k8s_%s_%s_%s_u_a",
	     jn_s ?: "",
	     jsn_s ?: (jhn_s ?: ""),
	     jsns_s ?: "");
    // and assign to hostname
    setContainerHostname(mod, container, compoundName);

    // next gather the latest metrics for this container
    cJSON *jcpu = cJSON_GetObjectItem(jmetrics, "Cpu");
    if(jcpu) {
      // TODO: get status from data.  With containerd it is the Process Status string
      container->state = SFL_VIR_DOMAIN_RUNNING;
      
      cJSON *jcputime = cJSON_GetObjectItem(jcpu, "CpuTime");
      if(jcputime) {
	container->cpu_total = jcputime->valuedouble;
      }
      cJSON *jcpucount = cJSON_GetObjectItem(jcpu, "CpuCount");
      if(jcpucount)
	container->cpu_count = jcpucount->valueint;
    }
    cJSON *jmem = cJSON_GetObjectItem(jmetrics, "Mem");
    if(jmem) {
      cJSON *jm = cJSON_GetObjectItem(jmem, "Memory");
      if(jm)
	container->mem_usage = jm->valuedouble; // TODO: units?
      cJSON *jmm = cJSON_GetObjectItem(jmem, "MaxMemory");
      if(jmm)
	container->memoryLimit = jmm->valuedouble; // TODO: units?
    }
    
    cJSON *jnet = cJSON_GetObjectItem(jmetrics, "Net");
    if(jnet) {
    }
    cJSON *jdsk = cJSON_GetObjectItem(jmetrics, "Dsk");
    if(jdsk) {
      cJSON *jrd_req = cJSON_GetObjectItem(jdsk, "Rd_req");
      cJSON *jwr_req = cJSON_GetObjectItem(jdsk, "Wr_req");
      cJSON *jrd_bytes = cJSON_GetObjectItem(jdsk, "Rd_bytes");
      cJSON *jwr_bytes = cJSON_GetObjectItem(jdsk, "Wr_bytes");
      if(jrd_req)
	container->dsk.rd_req = jrd_req->valuedouble;
      if(jwr_req)
	container->dsk.wr_req = jwr_req->valuedouble;
      if(jrd_bytes)
	container->dsk.rd_bytes = jrd_bytes->valuedouble;
      if(jwr_bytes)
	container->dsk.wr_bytes = jwr_bytes->valuedouble;
    }

    HSPVMState_POD *pod = NULL;
    if(isSandbox) {
      // For sandbox container we allocate a datasource that sends counters,
      // We also detect cgroup,network namespace interfaces and GPU devices.
      pod = getPod(mod, jid->valuestring, compoundSandbox, YES);
      if(pod) {
	// We have the pid, so we can probe for the MAC and peer-ifIndex
	// see if spacing the VNIC refresh reduces load
	time_t now_mono = mdata->pollBus->now.tv_sec;

	if(pod->last_vnic == 0
	   || (now_mono - pod->last_vnic) > HSP_VNIC_REFRESH_TIMEOUT) {
	  pod->last_vnic = now_mono;
	  updatePodAdaptors(mod, pod);
	}

	if(pod->last_cgroup == 0
	   || (now_mono - pod->last_cgroup) > HSP_CGROUP_REFRESH_TIMEOUT) {
	  pod->last_cgroup = now_mono;
	  updatePodCgroupPaths(mod, pod);
	}

	cJSON *jenv = cJSON_GetObjectItem(jmetrics, "Env");
	if(jenv
	   && pod->gpu_dev == NO)
	  readPodGPUsFromEnv(mod, pod, jenv);
	
	if(pod->cgroup_devices)
	  readPodGPUsFromDev(mod, pod);

	// send the counter sample right away
	// (the Go program has read /etc/hsflowd.auto to get the
	// polling interval, so it is already handling the polling
	// periodicity for us).
	getCounters_POD(mod, pod);
	// maybe this was the last one?
	if(podDone(mod, pod)) {
	  removeAndFreeVM_POD(mod, pod);
	}
	else {
	  // the sandbox container should also be in the list of
	  // containers for the pod.... I think.  It might make the
	  // stats easier to understand if we don't do this.  For
	  // example,  the sanbox container might add a CPU core
	  // to the count?
	  podAddContainer(mod, pod, container);
	}
      }
    }
    else {
      // not sandbox - regular container - find sandbox and add this to it.
      // Note that if we learn about the pod before we learn about the
      // sandbox then we will lose one polling cycle and the container
      // will only be added to the pod the next time it updates.  To avoid
      // that we could either look to see if the sandbox id is in the
      // JSON every time,  or just use the compoundSanbox string as the
      // only key for pods, because then we can create the pod and send
      // counters for it as soon as we see the first container from it.
      // At least I think we can do that.  Not sure it's really necessary,
      // however. Seems safer to only create the datasource when we have
      // the JSON for the sandbox.
      pod = getPodBySandbox(mod, compoundSandbox);
      if(pod)
	podAddContainer(mod, pod, container);
    }
  }
  
  static void readContainerData(EVMod *mod, char *str, void *magic) {
    // HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    int prefixLen = strlen(HSP_CONTAINERD_DATAPREFIX);
    if(memcmp(str, HSP_CONTAINERD_DATAPREFIX, prefixLen) == 0) {
      cJSON *top = cJSON_Parse(str + prefixLen);
      readContainerJSON(mod, top, magic);
      cJSON_Delete(top);
    }
  }
  
  static void readContainerCB(EVMod *mod, EVSocket *sock, EnumEVSocketReadStatus status, void *magic) {
    // HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    switch(status) {
    case EVSOCKETREAD_AGAIN:
      break;
    case EVSOCKETREAD_STR:
      // UTStrBuf_chomp(sock->ioline);
      myDebug(1, "readContainerCB: %s", UTSTRBUF_STR(sock->ioline));
      readContainerData(mod, UTSTRBUF_STR(sock->ioline), magic);
      UTStrBuf_reset(sock->ioline);
      break;
    case EVSOCKETREAD_EOF:
      myDebug(1, "readContainerCB EOF");
      break;
    case EVSOCKETREAD_BADF:
      myDebug(1, "readContainerCB BADF");
      break;
    case EVSOCKETREAD_ERR:
      myDebug(1, "readContainerCB ERR");
      break;
    }
  }
  
  /*_________________---------------------------__________________
    _________________    evt_flow_sample        __________________
    -----------------___________________________------------------
    Packet Bus
  */

  static uint32_t containerDSByMAC(EVMod *mod, SFLMacAddress *mac) {
    HSP *sp = (HSP *)EVROOTDATA(mod);
    SFLAdaptor *adaptor = adaptorByMac(sp, mac);
    if(adaptor) {
      uint32_t c_dsi = ADAPTOR_NIO(adaptor)->container_dsIndex;
      myDebug(2, "containerDSByMAC matched %s ds=%u\n", adaptor->deviceName, c_dsi);
      // make sure it wasn't marked as "non-unique"
      if(c_dsi != 0xFFFFFFFF)
	return c_dsi;
    }
    return 0;
  }

  static uint32_t containerDSByIP(EVMod *mod, SFLAddress *ipAddr) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSPVNIC search = { };
    search.ipAddr = *ipAddr;
    HSPVNIC *vnic = UTHashGet(mdata->vnicByIP, &search);
    if(vnic) {
      myDebug(2, "VNIC: got src %s (unique=%s, ds=%u)\n",
	      vnic->c_hostname,
	      vnic->unique ? "YES" : "NO",
	      vnic->dsIndex);
      if(vnic->unique)
	return vnic->dsIndex;
    }
    return 0;
  }
  
  static bool lookupContainerDS(EVMod *mod, HSPPendingSample *ps, uint32_t *p_src_dsIndex, uint32_t *p_dst_dsIndex) {
    // start with the one most likely to match
    // e.g. in Kubernetes with Calico IPIP or VXLAN this will be the innerIP:
    if(ps->gotInnerIP) {
      char sbuf[51],dbuf[51];
      *p_src_dsIndex = containerDSByIP(mod, &ps->src_1);
      *p_dst_dsIndex = containerDSByIP(mod, &ps->dst_1);
      
      myDebug(3, "lookupContainerDS: search by inner IP: src=%s dst=%s srcDS=%u dstDS=%u",
	      SFLAddress_print(&ps->src_1, sbuf, 50),
	      SFLAddress_print(&ps->dst_1, dbuf, 50),
	      *p_src_dsIndex,
	      *p_dst_dsIndex);
      
      if(*p_src_dsIndex || *p_dst_dsIndex)
	return YES;
    }
    if(ps->gotInnerMAC) {
      *p_src_dsIndex = containerDSByMAC(mod, &ps->macsrc_1);
      *p_dst_dsIndex = containerDSByMAC(mod, &ps->macdst_1);
      if(*p_src_dsIndex || *p_dst_dsIndex)
	return YES;
    }
    if(ps->l3_offset) {
      // outer IP
      *p_src_dsIndex = containerDSByIP(mod, &ps->src);
      *p_dst_dsIndex = containerDSByIP(mod, &ps->dst);
      if(*p_src_dsIndex || *p_dst_dsIndex)
	return YES;
    }
    if(ps->hdr_protocol == SFLHEADER_ETHERNET_ISO8023) {
      // outer MAC
      *p_src_dsIndex = containerDSByMAC(mod, &ps->macsrc);
      *p_dst_dsIndex = containerDSByMAC(mod, &ps->macdst);
      if(*p_src_dsIndex || *p_dst_dsIndex)
	return YES;
    }
    return NO;
  }
  
  static void evt_flow_sample(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    // HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSPPendingSample *ps = (HSPPendingSample *)data;
    decodePendingSample(ps);
    uint32_t src_dsIndex=0, dst_dsIndex=0;
    if(lookupContainerDS(mod, ps, &src_dsIndex, &dst_dsIndex)) {
      SFLFlow_sample_element *entElem = pendingSample_calloc(ps, sizeof(SFLFlow_sample_element));
      entElem->tag = SFLFLOW_EX_ENTITIES;
      if(src_dsIndex
	 && src_dsIndex != 0xFFFFFFFF) {
	entElem->flowType.entities.src_dsClass = SFL_DSCLASS_LOGICAL_ENTITY;
	entElem->flowType.entities.src_dsIndex = src_dsIndex;
      }
      if(dst_dsIndex
	 && dst_dsIndex != 0xFFFFFFFF) {
	entElem->flowType.entities.dst_dsClass = SFL_DSCLASS_LOGICAL_ENTITY;
	entElem->flowType.entities.dst_dsIndex = dst_dsIndex;
      }
      SFLADD_ELEMENT(ps->fs, entElem);
    }
  }

  /*_________________---------------------------__________________
    _________________    evt_cfg_done           __________________
    -----------------___________________________------------------
  */

  static void readCB(EVMod *mod, EVSocket *sock, void *magic) {
    EVSocketReadLines(mod, sock, readContainerCB, YES, magic);
  }

  static void evt_cfg_done(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);
    mdata->configRevisionNo = sp->revisionNo;
  }

  /*_________________---------------------------__________________
    _________________    tick,tock              __________________
    -----------------___________________________------------------
  */

  static void evt_tick(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    //HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
  }

  static void evt_tock(EVMod *mod, EVEvent *evt, void *data, size_t dataLen) {
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    if(mdata->configRevisionNo
       && mdata->readerPid == 0) {
      // Could pass debugLevel to reader like this:
      // char level[16];
      // snprintf(level, 16, "%u", getDebug());
      // char *cmd[] = { HSP_CONTAINERD_READER, "--debugLevel", level,  NULL };
      // but can always debug reader separately, so just invoke it like this:
      char *cmd[] = { HSP_CONTAINERD_READER, NULL };
      mdata->readerPid = EVBusExec(mod, mdata->pollBus, mdata, cmd, readCB);
    }
  }

  /*_________________---------------------------__________________
    _________________    module init            __________________
    -----------------___________________________------------------
  */

  void mod_k8s(EVMod *mod) {
    mod->data = my_calloc(sizeof(HSP_mod_K8S));
    HSP_mod_K8S *mdata = (HSP_mod_K8S *)mod->data;
    HSP *sp = (HSP *)EVROOTDATA(mod);

    struct stat statBuf;
    if(sp->docker.docker == YES
       && stat("/var/run/docker.sock", &statBuf) == 0) {
      myDebug(1, "not enabling mod_k8s because mod_docker is running and docker.sock is present");
      return;
    }

    // ask to retain root privileges
    retainRootRequest(mod, "needed by mod_k8s to access containerd.sock");
    retainRootRequest(mod, "needed by mod_k8s to probe for adaptors in other namespaces");

    requestVNodeRole(mod, HSP_VNODE_PRIORITY_CONTAINERD);

    buildRegexPatterns(mod);
    mdata->vmsByID = UTHASH_NEW(HSPVMState_POD, id, UTHASH_SKEY);
    mdata->vmsBySandbox = UTHASH_NEW(HSPVMState_POD, sandbox, UTHASH_SKEY);
    mdata->containersByID = UTHASH_NEW(HSPK8sContainer, id, UTHASH_SKEY);
    mdata->cgroupPathIdx = -1;
    
    // register call-backs
    mdata->pollBus = EVGetBus(mod, HSPBUS_POLL, YES);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, EVEVENT_TICK), evt_tick);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, EVEVENT_TOCK), evt_tock);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_CONFIG_DONE), evt_cfg_done);
    EVEventRx(mod, EVGetEvent(mdata->pollBus, HSPEVENT_HOST_COUNTER_SAMPLE), evt_host_cs);

    if(sp->containerd.markTraffic) {
      EVBus *packetBus = EVGetBus(mod, HSPBUS_PACKET, YES);
      EVEventRx(mod, EVGetEvent(packetBus, HSPEVENT_FLOW_SAMPLE), evt_flow_sample);
      mdata->vnicByIP = UTHASH_NEW(HSPVNIC, ipAddr, UTHASH_SYNC); // need sync (poll + packet thread)

      // learn my own namespace inode from /proc/self/ns/net
      if(stat("/proc/self/ns/net", &mdata->myNS) == 0)
	myDebug(1, "my namespace dev.inode == %u.%u",
		mdata->myNS.st_dev,
		mdata->myNS.st_ino);
    }
  }

#if defined(__cplusplus)
} /* extern "C" */
#endif
