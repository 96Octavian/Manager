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

/* Hold process name and PID */
typedef struct pids {
	char name[CHUNK];
	int pid;
	struct pids * next;
} pids_t;

/* Check if processes are running */
int checker(pids_t **head, char *dirname) {
	if (kill((*head)->pid, 0) == 0) return 0;	// Process is running or a zombie
	else if (errno == ESRCH) {
		
		/* No process with the given pid is running */
		int i, num = 0;
		size_t len;
		char *res = NULL;
		FILE *fp;
		char *path = malloc(strlen(dirname) + strlen((*head)->name) + 2);

		sprintf(path, "%s/%s", dirname, (*head)->name);
		fp = popen(path, "r");
		free(path);
		if (fp == NULL) return 2;	// File not found
		while ((num = reader(fp, &res, CHUNK))) {
			if (pclose(fp) / 256 == 127) return 2;	//File not found
			return (num*(-1)) - 1;
		}
		if (pclose(fp) / 256 == 127) return 2;	//File not found

		res[strcspn(res, "\n")] = 0;
		len = strlen(res);
		num = 0;
		for (i = 0; (unsigned int)i < len; i++) num = num * 10 + (res[i] - '0');
		(*head)->pid = num;
		free(res);
		return 1;
	}
	else return -1;	// some other error... use perror("...") or strerror(errno) to report
}

/* First function to be called, finds scripts and starts them */
int starter(pids_t **head, char *dirname) {
	DIR *pDir;
	struct dirent *pDirent;
	FILE *fp;
	char *res, *path;
	pids_t *current;
	int len, i, num;

	/* Open the given directory */
	pDir = opendir(dirname);
	if (pDir == NULL) {
		syslog(LOG_ERR, "Cannot open directory \"%s\"\n", dirname);
		closedir(pDir);
		return 0;
	}

	while ((pDirent = readdir(pDir)) != NULL) {
		if (pDirent->d_name[0] == '.') continue;

		path = malloc(strlen(dirname) + strlen(pDirent->d_name) + 2);
		sprintf(path, "%s/%s", dirname, pDirent->d_name);

		fp = popen(path, "r");
		free(path);
		if (fp == NULL) {
			syslog(LOG_ERR, "File not found");
			continue;
		}
		while ((num = reader(fp, &res, CHUNK))) {
			syslog(LOG_ERR, "Error reading output: %d", num);
			continue;
		}
		if (pclose(fp) / 256 == 127) {
			syslog(LOG_ERR, "File not found");
			continue;
		}

		res[strcspn(res, "\n")] = 0;
		len = strlen(res);
		num = 0;
		for (i = 0; i < len; i++) {
			num = num * 10 + (res[i] - '0');
		}
		free(res);

		if (*head == NULL) {
			*head = malloc(sizeof(pids_t));
			if (*head == NULL) {
				syslog(LOG_ERR, "Cannot create process list");
				closedir(pDir);
				return 0;
			}
			current = *head;
		}
		else {
			current->next = malloc(sizeof(pids_t));
			if (current->next == NULL) {
				syslog(LOG_ERR, "Cannot create process entry");
				continue;
			}
			current = current->next;
		}

		snprintf(current->name, CHUNK, "%s", pDirent->d_name);
		current->pid = num;
		current->next = NULL;

	}
	free(pDirent);
	closedir(pDir);
	return 1;
}

/* Check if a new scripts exists in the directory */
int updater(pids_t **head, char *dirname) {
	syslog(LOG_DEBUG, "Updating...");
	DIR *pDir;
	struct dirent *pDirent;
	pids_t *current;

	pDir = opendir(dirname);
	while ((pDirent = readdir(pDir)) != NULL) {
		if (pDirent->d_name[0] == '.') continue;

		if (*head == NULL) {
			*head = malloc(sizeof(pids_t));
			if (*head == NULL) {
				syslog(LOG_ERR, "Cannot create process entry");
				continue;
			}
			current = *head;
			snprintf(current->name, CHUNK, "%s", pDirent->d_name);
			current->pid = 32769;
			current->next = NULL;
			syslog(LOG_WARNING, "Added process %s as head", current->name);
		}

		else {
			current = *head;
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
	pids_t *removed, *current, *head = NULL;
	char directory[] = "startup_scripts";
	openlog("Manager", LOG_PERROR | LOG_PID | LOG_NDELAY, 0);
	setlogmask(LOG_UPTO(LOG_DEBUG));

	/* Obtain initial list of processes and start them */
	if (starter(&head, directory) == 0) {
		fprintf(stderr, "Error starting scripts\n");
		exit(EXIT_FAILURE);
	}

	while (1) { // Keep this program alive
		/* Check if list changed and if processes are still running. If they stopped, restart them */
		updater(&head, directory);
		current = head;
		syslog(LOG_INFO, "Loop");
		while (current != NULL) {
			syslog(LOG_NOTICE, "Testing %s with PID %d... ", current->name, current->pid);

			proc_status = checker(&current, directory);
			if (proc_status == 0) {
				syslog(LOG_NOTICE, "%s running OK", current->name);
			}
			else if (proc_status == 1) {
				syslog(LOG_WARNING, "%s restarted. PID %d\n", current->name, current->pid);
			}
			else if (proc_status == 2) {
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
			else if (proc_status == -1) {
				syslog(LOG_ERR, "Error: %s", strerror(errno));
			}
			else if (proc_status < -1) {
				syslog(LOG_WARNING, "Error parsing output from %s", current->name);
			}
			if (proc_status != 2) current = current->next;
			sleep(1);
		}
		sleep(5);
	}
	return 0;
}
