/**
 * collectd - src/processes.c
 * Copyright (C) 2005  Lyonel Vincent
 * Copyright (C) 2006  Florian Forster (Mach code)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Lyonel Vincent <lyonel at ezix.org>
 *   Florian octo Forster <octo at verplant.org>
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "utils_debug.h"
#include "configfile.h"

/* Include header files for the mach system, if they exist.. */
#if HAVE_THREAD_INFO
#  if HAVE_MACH_MACH_INIT_H
#    include <mach/mach_init.h>
#  endif
#  if HAVE_MACH_HOST_PRIV_H
#    include <mach/host_priv.h>
#  endif
#  if HAVE_MACH_MACH_ERROR_H
#    include <mach/mach_error.h>
#  endif
#  if HAVE_MACH_MACH_HOST_H
#    include <mach/mach_host.h>
#  endif
#  if HAVE_MACH_MACH_PORT_H
#    include <mach/mach_port.h>
#  endif
#  if HAVE_MACH_MACH_TYPES_H
#    include <mach/mach_types.h>
#  endif
#  if HAVE_MACH_MESSAGE_H
#    include <mach/message.h>
#  endif
#  if HAVE_MACH_PROCESSOR_SET_H
#    include <mach/processor_set.h>
#  endif
#  if HAVE_MACH_TASK_H
#    include <mach/task.h>
#  endif
#  if HAVE_MACH_THREAD_ACT_H
#    include <mach/thread_act.h>
#  endif
#  if HAVE_MACH_VM_REGION_H
#    include <mach/vm_region.h>
#  endif
#  if HAVE_MACH_VM_MAP_H
#    include <mach/vm_map.h>
#  endif
#  if HAVE_MACH_VM_PROT_H
#    include <mach/vm_prot.h>
#  endif
/* #endif HAVE_THREAD_INFO */

#elif KERNEL_LINUX
#  if HAVE_LINUX_CONFIG_H
#    include <linux/config.h>
#  endif
#  ifndef CONFIG_HZ
#    define CONFIG_HZ 100
#  endif
#endif /* KERNEL_LINUX */

#define MODULE_NAME "processes"

#if HAVE_THREAD_INFO || KERNEL_LINUX
# define PROCESSES_HAVE_READ 1
#else
# define PROCESSES_HAVE_READ 0
#endif

#define BUFSIZE 256

static char *processes_file = "processes.rrd";
static char *processes_ds_def[] =
{
	"DS:running:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	"DS:sleeping:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	"DS:zombies:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	"DS:stopped:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	"DS:paging:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	"DS:blocked:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	NULL
};
static int processes_ds_num = 6;

static char *ps_rss_file = "processes/ps_rss-%s.rrd";
static char *ps_rss_ds_def[] =
{
	/* max = 2^63 - 1 */
	"DS:byte:GAUGE:"COLLECTD_HEARTBEAT":0:9223372036854775807",
	NULL
};
static int ps_rss_ds_num = 1;

static char *ps_cputime_file = "processes/ps_cputime-%s.rrd";
static char *ps_cputime_ds_def[] =
{
	/* 1 second in user-mode per second ought to be enough.. */
	"DS:user:COUNTER:"COLLECTD_HEARTBEAT":0:1000000",
	"DS:syst:COUNTER:"COLLECTD_HEARTBEAT":0:1000000",
	NULL
};
static int ps_cputime_ds_num = 2;

static char *ps_count_file = "processes/ps_count-%s.rrd";
static char *ps_count_ds_def[] =
{
	"DS:processes:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	"DS:threads:GAUGE:"COLLECTD_HEARTBEAT":0:65535",
	NULL
};
static int ps_count_ds_num = 2;

static char *config_keys[] =
{
	"CollectName",
	NULL
};
static int config_keys_num = 1;

typedef struct procstat
{
#define PROCSTAT_NAME_LEN 256
	char               name[PROCSTAT_NAME_LEN];
	unsigned int       num_proc;
	unsigned int       num_lwp;
	unsigned long      vmem_rss;
	unsigned long      vmem_minflt;
	unsigned long      vmem_majflt;
	unsigned long long cpu_user;
	unsigned long long cpu_system;
	struct procstat   *next;
} procstat_t;

static procstat_t *list_head_g = NULL;

#if HAVE_THREAD_INFO
static mach_port_t port_host_self;
static mach_port_t port_task_self;

static processor_set_name_array_t pset_list;
static mach_msg_type_number_t     pset_list_len;
/* #endif HAVE_THREAD_INFO */

#elif KERNEL_LINUX
static long pagesize_g;
#endif /* KERNEL_LINUX */

static procstat_t *ps_list_append (procstat_t *list, const char *name)
{
	procstat_t *new;
	procstat_t *ptr;

	if ((new = (procstat_t *) malloc (sizeof (procstat_t))) == NULL)
		return (NULL);
	memset (new, 0, sizeof (procstat_t));
	strncpy (new->name, name, PROCSTAT_NAME_LEN);

	for (ptr = list; ptr != NULL; ptr = ptr->next)
		if (ptr->next == NULL)
			break;

	if (ptr != NULL)
		ptr->next = new;

	return (new);
}

static void ps_list_add (procstat_t *list, procstat_t *entry)
{
	procstat_t *ptr;

	ptr = list;
	while ((ptr != NULL) && (strcmp (ptr->name, entry->name) != 0))
		ptr = ptr->next;

	if (ptr == NULL)
		return;

	ptr->num_proc    += entry->num_proc;
	ptr->num_lwp     += entry->num_lwp;
	ptr->vmem_rss    += entry->vmem_rss;
	ptr->vmem_minflt += entry->vmem_minflt;
	ptr->vmem_majflt += entry->vmem_majflt;
	ptr->cpu_user    += entry->cpu_user;
	ptr->cpu_system  += entry->cpu_system;
}

static void ps_list_reset (procstat_t *ps)
{
	while (ps != NULL)
	{
		ps->num_proc    = 0;
		ps->num_lwp     = 0;
		ps->vmem_rss    = 0;
		ps->vmem_minflt = 0;
		ps->vmem_majflt = 0;
		ps->cpu_user    = 0;
		ps->cpu_system  = 0;
		ps = ps->next;
	}
}

static int ps_config (char *key, char *value)
{
	if (strcasecmp (key, "CollectName") == 0)
	{
		procstat_t *entry;

		entry = ps_list_append (list_head_g, value);
		if (entry == NULL)
		{
			syslog (LOG_ERR, "processes plugin: ps_list_append failed.");
			return (1);
		}
		if (list_head_g == NULL)
			list_head_g = entry;
	}
	else
	{
		return (-1);
	}

	return (0);
}

static void ps_init (void)
{
#if HAVE_THREAD_INFO
	kern_return_t status;

	port_host_self = mach_host_self ();
	port_task_self = mach_task_self ();

	if (pset_list != NULL)
	{
		vm_deallocate (port_task_self,
				(vm_address_t) pset_list,
				pset_list_len * sizeof (processor_set_t));
		pset_list = NULL;
		pset_list_len = 0;
	}

	if ((status = host_processor_sets (port_host_self,
					&pset_list,
				       	&pset_list_len)) != KERN_SUCCESS)
	{
		syslog (LOG_ERR, "host_processor_sets failed: %s\n",
			       	mach_error_string (status));
		pset_list = NULL;
		pset_list_len = 0;
		return;
	}
/* #endif HAVE_THREAD_INFO */

#elif KERNEL_LINUX
	pagesize_g = sysconf(_SC_PAGESIZE);
	DBG ("pagesize_g = %li; CONFIG_HZ = %i;",
			pagesize_g, CONFIG_HZ);
#endif /* KERNEL_LINUX */

	return;
}

static void ps_write (char *host, char *inst, char *val)
{
	rrd_update_file (host, processes_file, val,
			processes_ds_def, processes_ds_num);
}

static void ps_rss_write (char *host, char *inst, char *val)
{
	char filename[256];
	int status;

	status = snprintf (filename, 256, ps_rss_file, inst);
	if ((status < 1) || (status >= 256))
		return;

	rrd_update_file (host, filename, val, ps_rss_ds_def, ps_rss_ds_num);
}

static void ps_cputime_write (char *host, char *inst, char *val)
{
	char filename[256];
	int status;

	status = snprintf (filename, 256, ps_cputime_file, inst);
	if ((status < 1) || (status >= 256))
		return;

	DBG ("host = %s; filename = %s; val = %s;",
			host, filename, val);
	rrd_update_file (host, filename, val,
			ps_cputime_ds_def, ps_cputime_ds_num);
}

static void ps_count_write (char *host, char *inst, char *val)
{
	char filename[256];
	int status;

	status = snprintf (filename, 256, ps_count_file, inst);
	if ((status < 1) || (status >= 256))
		return;

	DBG ("host = %s; filename = %s; val = %s;",
			host, filename, val);
	rrd_update_file (host, filename, val,
			ps_count_ds_def, ps_count_ds_num);
}

#if PROCESSES_HAVE_READ
static void ps_submit (int running,
		int sleeping,
		int zombies,
		int stopped,
		int paging,
		int blocked)
{
	char buf[BUFSIZE];

	if (snprintf (buf, BUFSIZE, "%u:%i:%i:%i:%i:%i:%i",
				(unsigned int) curtime,
				running, sleeping, zombies, stopped, paging,
				blocked) >= BUFSIZE)
		return;

	DBG ("running = %i; sleeping = %i; zombies = %i; stopped = %i; paging = %i; blocked = %i;",
			running, sleeping, zombies, stopped, paging, blocked);

	plugin_submit (MODULE_NAME, "-", buf);
}

static void ps_submit_proc (procstat_t *ps)
{
	char buffer[64];

	if (ps == NULL)
		return;

	snprintf (buffer, 64, "%u:%lu",
			(unsigned int) curtime,
			ps->vmem_rss);
	buffer[63] = '\0';
	plugin_submit ("ps_rss", ps->name, buffer);

	snprintf (buffer, 64, "%u:%u:%u",
			(unsigned int) curtime,
			/* Make the counter overflow */
			(unsigned int) (ps->cpu_user   & 0xFFFFFFFF),
			(unsigned int) (ps->cpu_system & 0xFFFFFFFF));
	buffer[63] = '\0';
	plugin_submit ("ps_cputime", ps->name, buffer);

	snprintf (buffer, 64, "%u:%u:%u",
			(unsigned int) curtime,
			ps->num_proc, ps->num_lwp);
	buffer[63] = '\0';
	plugin_submit ("ps_count", ps->name, buffer);

	DBG ("name = %s; num_proc = %i; num_lwp = %i; vmem_rss = %i; "
			"vmem_minflt = %i; vmem_majflt = %i; "
			"cpu_user = %i; cpu_system = %i;",
			ps->name, ps->num_proc, ps->num_lwp, ps->vmem_rss,
			ps->vmem_minflt, ps->vmem_majflt, ps->cpu_user,
			ps->cpu_system);

}

#if KERNEL_LINUX
static int *ps_read_tasks (int pid)
{
	int *list = NULL;
	int  list_size = 1; /* size of allocated space, in elements */
	int  list_len = 0;  /* number of currently used elements */

	char           dirname[64];
	DIR           *dh;
	struct dirent *ent;

	snprintf (dirname, 64, "/proc/%i/task", pid);
	dirname[63] = '\0';

	if ((dh = opendir (dirname)) == NULL)
	{
		syslog (LOG_NOTICE, "processes plugin: Failed to open directory `%s'",
				dirname);
		return (NULL);
	}

	while ((ent = readdir (dh)) != NULL)
	{
		if (!isdigit (ent->d_name[0]))
			continue;

		if ((list_len + 1) >= list_size)
		{
			int *new_ptr;
			int  new_size = 2 * list_size;
			/* Comes in sizes: 2, 4, 8, 16, ... */

			new_ptr = (int *) realloc (list, (size_t) (sizeof (int) * new_size));
			if (new_ptr == NULL)
			{
				if (list != NULL)
					free (list);
				syslog (LOG_ERR, "processes plugin: "
						"Failed to allocate more memory.");
				return (NULL);
			}

			list = new_ptr;
			list_size = new_size;

			memset (list + list_len, 0, sizeof (int) * (list_size - list_len));
		}

		list[list_len] = atoi (ent->d_name);
		if (list[list_len] != 0)
			list_len++;
	}

	closedir (dh);

	assert (list_len < list_size);
	assert (list[list_len] == 0);

	return (list);
}

int ps_read_process (int pid, procstat_t *ps, char *state)
{
	char  filename[64];
	char  buffer[1024];
	FILE *fh;

	char *fields[64];
	char  fields_len;

	int  *tasks;
	int   i;

	int   ppid;
	int   name_len;

	memset (ps, 0, sizeof (procstat_t));

	snprintf (filename, 64, "/proc/%i/stat", pid);
	filename[63] = '\0';

	if ((fh = fopen (filename, "r")) == NULL)
		return (-1);

	if (fgets (buffer, 1024, fh) == NULL)
	{
		fclose (fh);
		return (-1);
	}

	fclose (fh);

	fields_len = strsplit (buffer, fields, 64);
	if (fields_len < 24)
	{
		DBG ("`%s' has only %i fields..",
				filename, fields_len);
		return (-1);
	}
	else if (fields_len != 41)
	{
		DBG ("WARNING: (fields_len = %i) != 41", fields_len);
	}

	/* copy the name, strip brackets in the process */
	name_len = strlen (fields[1]) - 2;
	if ((fields[1][0] != '(') || (fields[1][name_len + 1] != ')'))
	{
		DBG ("No brackets found in process name: `%s'", fields[1]);
		return (-1);
	}
	fields[1] = fields[1] + 1;
	fields[1][name_len] = '\0';
	strncpy (ps->name, fields[1], PROCSTAT_NAME_LEN);

	ppid = atoi (fields[3]);

	if ((tasks = ps_read_tasks (pid)) == NULL)
	{
		DBG ("ps_read_tasks (%i) failed.", pid);
		return (-1);
	}

	*state = '\0';
	ps->num_lwp  = 0;
	ps->num_proc = 1;
	for (i = 0; tasks[i] != 0; i++)
		ps->num_lwp++;

	free (tasks);
	tasks = NULL;

	/* Leave the rest at zero if this is only an LWP */
	if (ps->num_proc == 0)
	{
		DBG ("This is only an LWP: pid = %i; name = %s;",
				pid, ps->name);
		return (0);
	}

	ps->vmem_minflt = atol  (fields[9]);
	ps->vmem_majflt = atol  (fields[11]);
	ps->cpu_user    = atoll (fields[13]);
	ps->cpu_system  = atoll (fields[14]);
	ps->vmem_rss    = atol  (fields[23]);
	
	/* Convert jiffies to useconds */
	ps->cpu_user   = ps->cpu_user   * 1000000 / CONFIG_HZ;
	ps->cpu_system = ps->cpu_system * 1000000 / CONFIG_HZ;
	ps->vmem_rss   = ps->vmem_rss * pagesize_g;

	*state = fields[2][0];

	/* success */
	return (0);
} /* int ps_read_process (...) */
#endif /* KERNEL_LINUX */

static void ps_read (void)
{
#if HAVE_THREAD_INFO
	kern_return_t            status;

	int                      pset;
	processor_set_t          port_pset_priv;

	int                      task;
	task_array_t             task_list;
	mach_msg_type_number_t   task_list_len;

	int                      thread;
	thread_act_array_t       thread_list;
	mach_msg_type_number_t   thread_list_len;
	thread_basic_info_data_t thread_data;
	mach_msg_type_number_t   thread_data_len;

	int running  = 0;
	int sleeping = 0;
	int zombies  = 0;
	int stopped  = 0;
	int blocked  = 0;

	/*
	 * The Mach-concept is a little different from the traditional UNIX
	 * concept: All the work is done in threads. Threads are contained in
	 * `tasks'. Therefore, `task status' doesn't make much sense, since
	 * it's actually a `thread status'.
	 * Tasks are assigned to sets of processors, so that's where you go to
	 * get a list.
	 */
	for (pset = 0; pset < pset_list_len; pset++)
	{
		if ((status = host_processor_set_priv (port_host_self,
						pset_list[pset],
						&port_pset_priv)) != KERN_SUCCESS)
		{
			syslog (LOG_ERR, "host_processor_set_priv failed: %s\n",
					mach_error_string (status));
			continue;
		}

		if ((status = processor_set_tasks (port_pset_priv,
						&task_list,
						&task_list_len)) != KERN_SUCCESS)
		{
			syslog (LOG_ERR, "processor_set_tasks failed: %s\n",
					mach_error_string (status));
			mach_port_deallocate (port_task_self, port_pset_priv);
			continue;
		}

		for (task = 0; task < task_list_len; task++)
		{
			status = task_threads (task_list[task], &thread_list,
					&thread_list_len);
			if (status != KERN_SUCCESS)
			{
				/* Apple's `top' treats this case a zombie. It
				 * makes sense to some extend: A `zombie'
				 * thread is nonsense, since the task/process
				 * is dead. */
				zombies++;
				DBG ("task_threads failed: %s",
						mach_error_string (status));
				if (task_list[task] != port_task_self)
					mach_port_deallocate (port_task_self,
							task_list[task]);
				continue; /* with next task_list */
			}

			for (thread = 0; thread < thread_list_len; thread++)
			{
				thread_data_len = THREAD_BASIC_INFO_COUNT;
				status = thread_info (thread_list[thread],
						THREAD_BASIC_INFO,
						(thread_info_t) &thread_data,
						&thread_data_len);
				if (status != KERN_SUCCESS)
				{
					syslog (LOG_ERR, "thread_info failed: %s\n",
							mach_error_string (status));
					if (task_list[task] != port_task_self)
						mach_port_deallocate (port_task_self,
								thread_list[thread]);
					continue; /* with next thread_list */
				}

				switch (thread_data.run_state)
				{
					case TH_STATE_RUNNING:
						running++;
						break;
					case TH_STATE_STOPPED:
					/* What exactly is `halted'? */
					case TH_STATE_HALTED:
						stopped++;
						break;
					case TH_STATE_WAITING:
						sleeping++;
						break;
					case TH_STATE_UNINTERRUPTIBLE:
						blocked++;
						break;
					/* There is no `zombie' case here,
					 * since there are no zombie-threads.
					 * There's only zombie tasks, which are
					 * handled above. */
					default:
						syslog (LOG_WARNING,
								"Unknown thread status: %s",
								thread_data.run_state);
						break;
				} /* switch (thread_data.run_state) */

				if (task_list[task] != port_task_self)
				{
					status = mach_port_deallocate (port_task_self,
							thread_list[thread]);
					if (status != KERN_SUCCESS)
						syslog (LOG_ERR, "mach_port_deallocate failed: %s",
								mach_error_string (status));
				}
			} /* for (thread_list) */

			if ((status = vm_deallocate (port_task_self,
							(vm_address_t) thread_list,
							thread_list_len * sizeof (thread_act_t)))
					!= KERN_SUCCESS)
			{
				syslog (LOG_ERR, "vm_deallocate failed: %s",
						mach_error_string (status));
			}
			thread_list = NULL;
			thread_list_len = 0;

			/* Only deallocate the task port, if it isn't our own.
			 * Don't know what would happen in that case, but this
			 * is what Apple's top does.. ;) */
			if (task_list[task] != port_task_self)
			{
				status = mach_port_deallocate (port_task_self,
						task_list[task]);
				if (status != KERN_SUCCESS)
					syslog (LOG_ERR, "mach_port_deallocate failed: %s",
							mach_error_string (status));
			}
		} /* for (task_list) */

		if ((status = vm_deallocate (port_task_self,
				(vm_address_t) task_list,
				task_list_len * sizeof (task_t))) != KERN_SUCCESS)
		{
			syslog (LOG_ERR, "vm_deallocate failed: %s",
					mach_error_string (status));
		}
		task_list = NULL;
		task_list_len = 0;

		if ((status = mach_port_deallocate (port_task_self, port_pset_priv))
				!= KERN_SUCCESS)
		{
			syslog (LOG_ERR, "mach_port_deallocate failed: %s",
					mach_error_string (status));
		}
	} /* for (pset_list) */

	ps_submit (running, sleeping, zombies, stopped, -1, blocked);
/* #endif HAVE_THREAD_INFO */

#elif KERNEL_LINUX
	int running  = 0;
	int sleeping = 0;
	int zombies  = 0;
	int stopped  = 0;
	int paging   = 0;
	int blocked  = 0;

	struct dirent *ent;
	DIR           *proc;
	int            pid;

	int        status;
	procstat_t ps;
	char       state;

	procstat_t *ps_ptr;

	running = sleeping = zombies = stopped = paging = blocked = 0;
	ps_list_reset (list_head_g);

	if ((proc = opendir ("/proc")) == NULL)
	{
		syslog (LOG_ERR, "Cannot open `/proc': %s", strerror (errno));
		return;
	}

	while ((ent = readdir (proc)) != NULL)
	{
		if (!isdigit (ent->d_name[0]))
			continue;

		if ((pid = atoi (ent->d_name)) < 1)
			continue;

		status = ps_read_process (pid, &ps, &state);
		if (status != 0)
		{
			DBG ("ps_read_process failed: %i", status);
			continue;
		}

		switch (state)
		{
			case 'R': running++;  break;
			case 'S': sleeping++; break;
			case 'D': blocked++;  break;
			case 'Z': zombies++;  break;
			case 'T': stopped++;  break;
			case 'W': paging++;   break;
		}

		if (list_head_g != NULL)
			ps_list_add (list_head_g, &ps);
	}

	closedir (proc);

	ps_submit (running, sleeping, zombies, stopped, paging, blocked);

	for (ps_ptr = list_head_g; ps_ptr != NULL; ps_ptr = ps_ptr->next)
		ps_submit_proc (ps_ptr);
#endif /* KERNEL_LINUX */
}
#else
# define ps_read NULL
#endif /* PROCESSES_HAVE_READ */

void module_register (void)
{
	plugin_register (MODULE_NAME, ps_init, ps_read, ps_write);
	plugin_register ("ps_rss", NULL, NULL, ps_rss_write);
	plugin_register ("ps_cputime", NULL, NULL, ps_cputime_write);
	plugin_register ("ps_count", NULL, NULL, ps_count_write);
	cf_register (MODULE_NAME, ps_config, config_keys, config_keys_num);
}

#undef BUFSIZE
#undef MODULE_NAME
