/*
  Check: a unit test framework for C
  Copyright (C) 2001, Arien Malec

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif
#ifndef WIFSIGNALED
# define WIFSIGNALED(stat_val)	\
	(((stat_val) & 255) != 255 && \
	 ((stat_val) & 255) != 0)
#endif
#ifndef WTERMSIG
# define WTERMSIG(stat_val)	((stat_val) & 255)
#endif
#ifndef WIFEXITED
# define WIFEXITED(stat_val)	(((stat_val) & 255) == 0)
#endif
#ifndef WEXITSTATUS
# define WEXITSTATUS(stat_val)	((unsigned)(stat_val) >> 8)
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <stdio.h>

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stdarg.h>
#endif

#include "error.h"
#include "list.h"
#include "check.h"
#include "check_impl.h"
#include "check_msg.h"
#include "check_log.h"

#ifndef USE_FORKWAITMSG
int nofork_exit_status;
#endif

static void srunner_run_tcase (SRunner *sr, TCase *tc);
static void srunner_add_failure (SRunner *sr, TestResult *tf);
static TestResult *tfun_run (int msqid, char *tcname, TF *tf);
static TestResult *receive_result_info (int msqid, int status, char *tcname,
					char *tfname);
static void receive_last_loc_info (int msqid, TestResult *tr);
static void receive_failure_info (int msqid, int status, TestResult *tr);
static List *srunner_resultlst (SRunner *sr);


#ifdef USE_FORKWAITMSG
static char *signal_msg (int sig);
#endif
static char *exit_msg (int exitstatus);
static int non_pass (int val);


SRunner *srunner_create (Suite *s)
{
  SRunner *sr = emalloc (sizeof(SRunner)); /* freed in srunner_free */
  sr->slst = list_create();
  list_add_end(sr->slst, s);
  sr->stats = emalloc (sizeof(TestStats)); /* freed in srunner_free */
  sr->stats->n_checked = sr->stats->n_failed = sr->stats->n_errors = 0;
  sr->resultlst = list_create();
  sr->log_fname = NULL;
  sr->loglst = NULL;
  return sr;
}

void srunner_add_suite (SRunner *sr, Suite *s)
{
  list_add_end(sr->slst, s);
}

void srunner_free (SRunner *sr)
{
  List *l;
  TestResult *tr;
  if (sr == NULL)
    return;
  
  free (sr->stats);
  list_free(sr->slst);

  l = sr->resultlst;
  for (list_front(l); !list_at_end(l); list_advance(l)) {
    tr = list_val(l);
    free(tr->file);
    if (tr->rtype == CRFAILURE || tr->rtype == CRERROR)
      free(tr->msg);
    free(tr);
  }
  list_free (sr->resultlst);

  free (sr);
} 



void srunner_run_all (SRunner *sr, int print_mode)
{
  List *slst;
  List *tcl;
  TCase *tc;
  if (sr == NULL)
    return;
  if (print_mode < 0 || print_mode >= CRLAST)
    eprintf("Bad print_mode argument to srunner_run_all: %d", print_mode);

  srunner_init_logging (sr, print_mode);

  log_srunner_start (sr);

  slst = sr->slst;
  
  for (list_front(slst); !list_at_end(slst); list_advance(slst)) {
    Suite *s = list_val(slst);
    
    log_suite_start (sr, s);

    tcl = s->tclst;
  
    for (list_front(tcl);!list_at_end (tcl); list_advance (tcl)) {
      tc = list_val (tcl);
      srunner_run_tcase (sr, tc);
    }
  }

  log_srunner_end (sr);

  srunner_end_logging (sr);
}

static void srunner_add_failure (SRunner *sr, TestResult *tr)
{
  sr->stats->n_checked++;
  list_add_end (sr->resultlst, tr);
  switch (tr->rtype) {
    
  case CRPASS:
    return;
  case CRFAILURE:
    sr->stats->n_failed++;
    return;
  case CRERROR:
    sr->stats->n_errors++;
    return;
  }
}

  
static void srunner_run_tcase (SRunner *sr, TCase *tc)
{
  List *tfl;
  TF *tfun;
  TestResult *tr;
  int msqid;

  if (tc->setup)
    tc->setup();
  msqid = create_msq();
  tfl = tc->tflst;
  
  for (list_front(tfl); !list_at_end (tfl); list_advance (tfl)) {
    tfun = list_val (tfl);
    tr = tfun_run (msqid, tc->name, tfun);
    srunner_add_failure (sr, tr);
    log_test_end(sr, tr);
  }
  delete_msq(msqid);
  if (tc->teardown)
    tc->teardown();
}

static void receive_last_loc_info (int msqid, TestResult *tr)
{
  LastLocMsg *lmsg;
  lmsg = receive_last_loc_msg (msqid);
  tr->file = last_loc_file (lmsg);
  tr->line = last_loc_line (lmsg);
  free (lmsg);
}  

static void receive_failure_info (int msqid, int status, TestResult *tr)
{
  FailureMsg *fmsg;
#ifdef USE_FORKWAITMSG
  if (WIFSIGNALED(status)) {
    tr->rtype = CRERROR;
    tr->msg = signal_msg (WTERMSIG(status));
    return;
  }
  
  if (WIFEXITED(status)) {
    
    if (WEXITSTATUS(status) == 0) {
      tr->rtype = CRPASS;
      /* TODO: It would be cleaner to strdup this &
	 not special case the free...*/
      tr->msg = "Test passed";
    }
    else {
      
      fmsg = receive_failure_msg (msqid);
      if (fmsg == NULL) { /* implies early exit */
	tr->rtype = CRERROR;
	tr->msg =  exit_msg (WEXITSTATUS(status));
      }
      else {
	tr->rtype = CRFAILURE;
	tr->msg = emalloc(strlen(fmsg->msg) + 1);
	strcpy (tr->msg, fmsg->msg);
	free (fmsg);
      }
    }
  } else {
    eprintf ("Bad status from wait() call\n");
  }
#else
  if (status == 0) {
    tr->rtype = CRPASS;
    tr->msg = "Test passed";
  }
  else {
    fmsg = receive_failure_msg (msqid);
    if (fmsg == NULL) { /* implies early exit */
      tr->rtype = CRERROR;
      tr->msg =  exit_msg (status);
    }
    else {
      tr->rtype = CRFAILURE;
      tr->msg = emalloc(strlen(fmsg->msg) + 1);
      strcpy (tr->msg, fmsg->msg);
      free (fmsg);
    }
  }
#endif
}

static TestResult *receive_result_info (int msqid, int status, char *tcname,
					char *tfname)
{
  TestResult *tr = emalloc (sizeof(TestResult));

  tr->tcname = tcname;
  tr->tfname = tfname;
  receive_last_loc_info (msqid, tr);
  receive_failure_info (msqid, status, tr);
  return tr;
}

static TestResult *tfun_run (int msqid, char *tcname, TF *tfun)
{
#ifdef USE_FORKWAITMSG
  pid_t pid;
#endif
  int status = 0;

#ifdef USE_FORKWAITMSG
  pid = fork();
  if (pid == -1)
     eprintf ("Unable to fork:");
  if (pid == 0) {
    tfun->fn(msqid);
    _exit(EXIT_SUCCESS);
  }
  (void) wait(&status);
#else
  nofork_exit_status = 0;
  tfun->fn(msqid);
  status = nofork_exit_status;
#endif
  return receive_result_info(msqid, status, tcname, tfun->name);
}




int srunner_ntests_failed (SRunner *sr)
{
  return sr->stats->n_failed + sr->stats->n_errors;
}

int srunner_ntests_run (SRunner *sr)
{
  return sr->stats->n_checked;
}

TestResult **srunner_failures (SRunner *sr)
{
  int i = 0;
  TestResult **trarray;
  List *rlst;
  trarray = malloc (sizeof(trarray[0]) * srunner_ntests_failed (sr));

  rlst = srunner_resultlst (sr);
  for (list_front(rlst); !list_at_end(rlst); list_advance(rlst)) {
    TestResult *tr = list_val(rlst);
    if (non_pass(tr->rtype))
      trarray[i++] = tr;
    
  }
  return trarray;
}

TestResult **srunner_results (SRunner *sr)
{
  int i = 0;
  TestResult **trarray;
  List *rlst;

  trarray = malloc (sizeof(trarray[0]) * srunner_ntests_run (sr));

  rlst = srunner_resultlst (sr);
  for (list_front(rlst); !list_at_end(rlst); list_advance(rlst)) {
    trarray[i++] = list_val(rlst);
  }
  return trarray;
}

static List *srunner_resultlst (SRunner *sr)
{
  return sr->resultlst;
}

char *tr_msg (TestResult *tr)
{
  return tr->msg;
}

int tr_lno (TestResult *tr)
{
  return tr->line;
}

char *tr_lfile (TestResult *tr)
{
  return tr->file;
}

int tr_rtype (TestResult *tr)
{
  return tr->rtype;
}

char *tr_tcname (TestResult *tr)
{
  return tr->tcname;
}

#ifdef USE_FORKWAITMSG
static char *signal_msg (int signal)
{
  char *msg = emalloc (CMAXMSG); /* free'd by caller */
  snprintf(msg, CMAXMSG, "Received signal %d", signal);
  return msg;
}
#endif

static char *exit_msg (int exitval)
{
  char *msg = emalloc(CMAXMSG); /* free'd by caller */
  snprintf(msg, CMAXMSG,
	   "Early exit with return value %d", exitval);
  return msg;
}

static int non_pass (int val)
{
  return val == CRFAILURE || val == CRERROR;
}

