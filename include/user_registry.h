#ifndef USER_REGISTRY_H
#define USER_REGISTRY_H

#include <stddef.h>
#include <sys/types.h>

#define USER_REGISTRY_MAX 64
#define REGISTRY_USERNAME_SIZE 32
#define REGISTRY_ACCOUNT_SIZE 7
#define REGISTRY_ROLE_SIZE 16

struct RegistryUser {
    char username[REGISTRY_USERNAME_SIZE];
    char account_id[REGISTRY_ACCOUNT_SIZE];
    char role[REGISTRY_ROLE_SIZE];
    off_t lock_offset;
};

int user_registry_init(void);
int user_registry_load(struct RegistryUser users[],
                       size_t capacity,
                       size_t *count);
int user_registry_add(const char *username,
                      const char *account_id,
                      const char *role,
                      off_t lock_offset);
int user_registry_set_role(const char *username, const char *role);
int user_registry_remove(const char *username);

#endif
