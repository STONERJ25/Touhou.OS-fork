#ifndef _NET_IF_H
#define _NET_IF_H

#define IF_NAMESIZE 16

struct if_nameindex {
    unsigned int  if_index;
    char         *if_name;
};

/* Interface indexes start at 1 (lo is 1); they are what sin6_scope_id carries for a link-local IPv6 address. */
unsigned int         if_nametoindex(const char *name);                 /* 0 (errno ENODEV) if there is no such interface */
char                *if_indextoname(unsigned int index, char *name);   /* NULL (errno ENXIO) if there is none */
struct if_nameindex *if_nameindex(void);                               /* an array ended by an entry with index 0 and name NULL */
void                 if_freenameindex(struct if_nameindex *list);

#endif
