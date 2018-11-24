#include <syslog.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "../useful.h"

#define CHUNK 16

enum {RUNNING_OK, RESTARTED, SCRIPT_NOT_FOUND, SOME_ERROR};

char directory[] = "startup_scripts";

/* Hold process name and PID */
typedef struct pid_s {
	char name[CHUNK];
	int pid;
	struct pid_s * next;
} pids_t;

/* Head of process list */
pids_t *head = NULL;

int start_process(pids_t **current, char *name) {
	int i, num = 0;
	size_t len;
	char *res = NULL;
	FILE *fp;
	char *path = malloc(strlen(directory) + strlen(name) + 2);

	sprintf(path, "%s/%s", directory, name);
	fp = popen(path, "r");
	free(path);
	if (fp == NULL) return SCRIPT_NOT_FOUND;	// File not found
	while ((num = reader(fp, &res, CHUNK))) {
		if (pclose(fp) / 256 == 127) return SCRIPT_NOT_FOUND;	//File not found
		return (num*(-1)) - 1;
	}
	if (pclose(fp) / 256 == 127) return SCRIPT_NOT_FOUND;	//File not found

	res[strcspn(res, "\n")] = 0;
	len = strlen(res);
	num = 0;
	for (i = 0; (unsigned int)i < len; i++) num = num * 10 + (res[i] - '0');
	(*current)->pid = num;
	free(res);
	return RUNNING_OK;
}

/* Check if processes are running */
int checker(pids_t **current) {
	if (kill((*current)->pid, 0) == 0) return RUNNING_OK;	// Process is running or a zombie
	else if (errno == ESRCH) {
		return start_process(current, (*current)->name);
	}
	else return SOME_ERROR;	// some other error... use perror("...") or strerror(errno) to report
}

/* Opens the directory and updates script list */
int lister(char *dirname) {
	DIR *pDir;
	struct dirent *pDirent;
	FILE *fp;
	char *res, *path;
	pids_t *current;

	/* Open the given directory */
	pDir = opendir(dirname);
	if (pDir == NULL) {
		syslog(LOG_ERR, "Cannot open directory \"%s\"\n", dirname);
		closedir(pDir);
		return 0;
	}

	if (head == NULL) {
		head = malloc(sizeof(pids_t));
		if (head == NULL) {
			syslog(LOG_ERR, "Cannot initiate list");
			free(pDirent);
			closedir();
			return 0;
		}
	}

	while ((pDirent = readdir(pDir)) != NULL) {
		if (pDirent->d_name[0] == '.') continue;

		current = head;
		while (current != NULL) {
			if (strcmp(pDirent->d_name, current->name) == 0) break;

			else if (current->next == NULL) {
				current->next = malloc(sizeof(pids_t));
				if (current->next == NULL) {
					syslog(LOG_ERR, "Cannot create process entry");
					continue;
				}
				current = current->next;
				snprintf(current->name, CHUNK, "%s", pDirent->d_name);
				current->pid = 32769;
				current->next = NULL;
				syslog(LOG_WARNING, "Added process %s", current->name);
				break;
			}
			current = current->next;
		}

	}
	free(pDirent);
	closedir(pDir);
	return 1;
}

/* Check if a new scripts exists in the directory */
int updater(char *dirname) {
	syslog(LOG_DEBUG, "Updating...");
	DIR *pDir;
	struct dirent *pDirent;
	pids_t *current;

	pDir = opendir(dirname);
	while ((pDirent = readdir(pDir)) != NULL) {
		if (pDirent->d_name[0] == '.') continue;

		if (head == NULL) {
			head = malloc(sizeof(pids_t));
			if (head == NULL) {
				syslog(LOG_ERR, "Cannot create process entry");
				continue;
			}
			current = head;
			snprintf(current->name, CHUNK, "%s", pDirent->d_name);
			current->pid = 32769;
			current->next = NULL;
			syslog(LOG_WARNING, "Added process %s as head", current->name);
		}

		else {
			current = head;
			while (current != NULL) {
				if (strcmp(pDirent->d_name, current->name) == 0) break;

				else if (current->next == NULL) {
					current->next = malloc(sizeof(pids_t));
					if (current->next == NULL) {
						syslog(LOG_ERR, "Cannot create process entry");
						continue;
					}
					current = current->next;
					snprintf(current->name, CHUNK, "%s", pDirent->d_name);
					current->pid = 32769;
					current->next = NULL;
					syslog(LOG_WARNING, "Added process %s", current->name);
				}
				current = current->next;
			}
		}

	}
	free(pDirent);
	closedir(pDir);
	return 1;
}

int main() {
	int proc_status;
	pids_t *removed, *current;
	char directory[] = "startup_scripts";
	openlog("Manager", LOG_PERROR | LOG_PID | LOG_NDELAY, 0);
	setlogmask(LOG_UPTO(LOG_WARNING));

	/* Obtain initial list of processes and start them */
	if (starter(directory) == 0) {
		fprintf(stderr, "Error starting scripts\n");
		exit(EXIT_FAILURE);
	}

	while (1) { // Keep this program alive
		/* Check if list changed and if processes are still running. If they stopped, restart them */
		updater(directory);
		current = head;
		syslog(LOG_INFO, "Loop");
		while (current != NULL) {
			syslog(LOG_NOTICE, "Testing %s with PID %d... ", current->name, current->pid);

			proc_status = checker(&current);
			if (proc_status == RUNNING_OK) {
				syslog(LOG_NOTICE, "%s running OK", current->name);
			}
			else if (proc_status == RESTARTED) {
				syslog(LOG_WARNING, "%s restarted. PID %d\n", current->name, current->pid);
			}
			else if (proc_status == SCRIPT_NOT_FOUND) {
				syslog(LOG_ERR, "%s not running, script not found", current->name);
				if (current == head) {
					head = head->next;
					free(current);
					syslog(LOG_WARNING, "Removed");
					current = head;
				}
				else {
					removed = current;
					current = head;
					while (current->next != removed) {
						current = current->next;
					}
					current->next = current->next->next;
					free(removed);
					syslog(LOG_WARNING, "Removed");
					current = current->next;
				}

			}
			else if (proc_status == SOME_ERROR) {
				syslog(LOG_ERR, "Error: %s", strerror(errno));
			}
			else if (proc_status < RUNNING_OK) {
				syslog(LOG_WARNING, "Error parsing output from %s", current->name);
			}
			if (proc_status != 2) current = current->next;
			sleep(1);
		}
		sleep(5);
	}
	return 0;
}
