#ifndef AUTH_H
#define AUTH_H

#define ENCRYPTION_PADDING 8
#define DECRYPTION_PADDING 8

char *sign_data(struct gale_id *eid,const char *data,const char *end);
char *encrypt_data(struct gale_id *eid,const char *data,const char *dend,
                   char *out,char **oend);

#endif