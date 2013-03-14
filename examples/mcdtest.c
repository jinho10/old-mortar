#include <stdio.h>
#include <string.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <math.h>

#include <mysql/mysql.h>

#include <linux/mcd.h>

/*
 * Time Measurement Related Functions
 */
int timeval_subtract(struct timeval *ret, struct timeval *e, struct timeval *s)
{
    long int diff;

    if ( ret == NULL || e == NULL || s == NULL ) return -1;

    diff = (e->tv_usec + 1E6 * e->tv_sec) - (s->tv_usec + 1E6 * s->tv_sec);
    ret->tv_sec = diff / 1E6;
    ret->tv_usec = diff % (int)1E6;

    return (diff<0);
}

void timeval_print(struct timeval *t)
{
    if ( t == NULL ) return;
    printf("%ld.%06ld sec\n", t->tv_sec, t->tv_usec);
}

int time_diff(struct timeval *s)
{
	struct timeval e;
	struct timeval d;

    if ( s == NULL ) return -1;

    gettimeofday(&e, NULL);
    timeval_subtract(&d, &e, s);
    timeval_print(&d);

    return 1;
}

void time_marking(struct timeval *t)
{
    if ( t == NULL ) return;
    gettimeofday(t, NULL);
}

/*
 * MySQL-Related Functions
 */
MYSQL *mysql_connect(char* server, char* user, char* passwd, char* db) 
{
    MYSQL *conn = mysql_init(NULL);

    if ( conn == NULL ) printf("another shit!\n");

    /* Connect to database */
    if ( !mysql_real_connect(conn, server, user, passwd, db, 0, NULL, 0) ) {
        printf("mysql_real_connect failed: %s\n", mysql_error(conn));
        return NULL;
    }

    return conn;
}

void mysql_disconnect(MYSQL* conn)
{
    mysql_close(conn);
}

/* TODO this is just reference, do not use it in reality
 * because it is not efficient at all
 */
int mysql_table_headers(MYSQL* conn, char* table_name) 
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    MYSQL_FIELD *field;
    int num_fields;

    char query[14 + strlen(table_name) + 1];

    sprintf(query, "SELECT * FROM %s", table_name);

    //printf("%s\n", query);
    
    mysql_query(conn, query);
    res = mysql_store_result(conn);
    num_fields = mysql_num_fields(res);

    while(field = mysql_fetch_field(res)) {
        printf("%s ", field->name);
    }
    printf("\n");

    mysql_free_result(res);

    return 0;
}

int mysql_inf_query(MYSQL* conn, char* query) 
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    int num_fields;

    if (mysql_query(conn, query)) {
        fprintf(stderr, "%s\n", mysql_error(conn));
        return -1;
    }

    res = mysql_use_result(conn);
    num_fields = mysql_num_fields(res);

    while ((row = mysql_fetch_row(res)) != NULL) {
        int i;
        for(i = 0; i < num_fields; i++) {
            printf("%s ", row[i] ? row[i] : NULL);
        }
        printf("\n");
    }

    mysql_free_result(res);

    return 0;
}

// could be text file, binary, image file
// field should be blob type
// TODO should set the rc value
int mysql_file_put(MYSQL* conn, char* table, char* fn)
{
    struct stat st;
    FILE *file;

    int file_size, ret_size, len;
    char *data, *chunk, *query;

    int rc = 0;

    stat(fn, &st);
    file_size = st.st_size;

    data = (char*) malloc(file_size);

    file = fopen(fn, "rb");
    ret_size = fread(data, 1, file_size, file);
    if ( ret_size != file_size ) {
        printf("File Read failed\n");
        goto out;
    }

    chunk = (char*) malloc(file_size*2 + 1); // needs optimization
    if ( chunk == NULL ) {
        printf("File Read failed\n");
        goto out;
    }

    query = (char*) malloc(file_size*5); // needs optimization
    if ( query == NULL ) {
        printf("File Read failed\n");
        goto out;
    }

    ret_size = mysql_real_escape_string(conn, chunk, data, file_size);
    char *stat = "INSERT INTO %s (k, v) VALUES('%s', '%s')";
    len = snprintf(query, strlen(stat) + strlen(table) + strlen(fn) + ret_size, stat, table, fn, chunk);

    if( mysql_real_query(conn, query, strlen(query)) )
    {
        fprintf(stderr, "Failed to insert row, Error: %s\n", mysql_error(conn));
        goto out;
    }

out:
    free(data);
    free(chunk);
    free(query);

    fclose(file);

    return rc;
}

int mysql_file_get(MYSQL* conn, char* table, char* key)
{
    MYSQL_RES *res;
    MYSQL_ROW row;

    char query[1000];
    int len, num_fields;
    int rc = 0;

    char *stat = "SELECT * FROM %s WHERE k = '%s'";
    len = snprintf(query, strlen(stat) + strlen(table) + strlen(key), stat, table, key);

    if( mysql_real_query(conn, query, strlen(query)) )
    {
        fprintf(stderr, "Failed to insert row, Error: %s\n", mysql_error(conn));
        return -1;
    }

    res = mysql_store_result(conn);
    num_fields = mysql_num_fields(res);
    while ((row = mysql_fetch_row(res)))
    {
        int i;
        for(i = 0; i < num_fields; i++)
        {
            printf("%s ", row[i] ? row[i] : "NULL");
        }
        printf("\n");
    }
    
    mysql_free_result(res);

    return rc;
}

int mysql_file_remove(MYSQL* conn, char* table, char* key)
{
    char query[1000];
    int len;
    int rc = 0;

    char *stat = "DELETE FROM %s WHERE k = '%s'";
    len = snprintf(query, strlen(stat) + strlen(table) + strlen(key), stat, table, key);

    if( mysql_real_query(conn, query, strlen(query)) )
    {
        fprintf(stderr, "Failed to insert row, Error: %s\n", mysql_error(conn));
        return -1;
    }

    return rc;
}


int mysql_op(char* cmd, char* key)
{
    // insert file
    MYSQL *conn = NULL;

    //conn = mysql_connect("localhost", "root", "qwer1234", "mcd");
    conn = mysql_connect("192.168.1.205", "root", "qwer1234", "test");
    if ( conn == NULL ) {
        printf("mysql_connect failed\n");
        return -1;
    }

    //mysql_table_headers(conn, "dummy");

    if ( !strcmp(cmd, "put") ) {
        if ( mysql_file_put(conn, "mcd", key) ) {
            printf("mysql_file_put failed \n");
            return -1;
        }
    } else if ( !strcmp(cmd, "get") ) {
        if ( mysql_file_get(conn, "mcd", key) ) {
            printf("mysql_file_get failed \n");
            return -1;
        }
    } else if ( !strcmp(cmd, "remove") ) {
        if ( mysql_file_remove(conn, "mcd", key) ) {
            printf("mysql_file_remove failed \n");
            return -1;
        }
    }

    mysql_disconnect(conn);

    return 0;
}

/*
 * File Operation-Related 
 */
int file_get(char* key)
{
    struct stat st;
    int val_size, read_size;
    int ret_val;
    char *buf;

    int rc = 0;

    FILE *file;

    stat(key, &st);
    val_size = st.st_size;

printf("%s\n", key);

    file = fopen(key, "r");
    if ( file == NULL ) {
        printf("file open failed\n");
        return -1;
    }

    buf = (char*) malloc(val_size);

    read_size = fread(buf, sizeof(char), val_size, file);
    if ( read_size != val_size ) {
        printf("File read failed\n");
        free(buf);
        return -1;
    }

    //printf("%s\n", buf);

    free(buf);
    fclose(file);

    return rc;
}

/*
 * Memcached Related Functions
 */
unsigned int getsize(int opt, char *key) 
{
    unsigned int ret_val = 0;
    mcd_verbose(XENMCD_cache_check, opt, strlen(key), key, 0, &ret_val, NULL);
	return ret_val;
}

int mcd_put(char* key)
{
    struct stat st;
    int val_size, read_size;
    int ret_val;
    char *buf;

    int rc = 0;

    FILE *file;

    stat(key, &st);
    val_size = st.st_size;

    file = fopen(key, "r");
    if ( file == NULL ) return -1;

    buf = (char*) malloc(val_size);

    read_size = fread(buf, sizeof(char), val_size, file);
    if ( read_size != val_size ) {
        printf("File read failed\n");
        free(buf);
        return -1;
    }
    mcd_verbose(XENMCD_cache_put, MCDOPT_shared, strlen(key), key, val_size, &ret_val, buf);

    if ( ret_val > 0 ) printf("%s successfully put\n", key);
    else printf("error code: %d\n", ret_val);

    free(buf);
    fclose(file);

    return rc;
}

int mcd_get(char* key)
{
	char *buf;
	int r_val_size;
	int i;

    r_val_size = getsize(MCDOPT_shared, key);

    buf = malloc(r_val_size + 1);

	mcd_verbose(XENMCD_cache_get, MCDOPT_shared, strlen(key), key, r_val_size, &r_val_size, buf);

    if ( r_val_size < 0 ) {
        printf("Get operation error: %d\n", r_val_size);
        return r_val_size;
    }

	printf("Value Size = %d\n", r_val_size);
	for(i = 0; i < r_val_size; i++) {
		printf("%c", buf[i]);
	}
	printf("\n");

    free(buf);

    return 0;
}

void mcd_check(char* key) 
{
	int ret_val = 0;

	mcd_verbose(XENMCD_cache_check, MCDOPT_shared, strlen(key), key, 0, &ret_val, NULL);

    if ( ret_val > 0 ) printf("%s exists\n", key);
    else printf("error code: %d\n", ret_val);
}

void flush(void) 
{
	int ret_val = 0;

    mcd_verbose(XENMCD_cache_flush, MCDOPT_shared, 0, NULL, 0, &ret_val, NULL);

    if ( ret_val >= 0 ) printf("flush succeeded\n");
    else printf("error code: %d\n", ret_val);
}

void mcd_rm(char* key)
{
	int ret_val = 0;

	mcd_verbose(XENMCD_cache_remove, MCDOPT_shared, strlen(key), key, 0, &ret_val, NULL);

    if ( ret_val > 0 ) printf("%s removed\n", key);
    else printf("error code: %d\n", ret_val);
}

int file_test(int argc, char* argv[])
{
    if ( argc < 4 ) {
        printf("usage: %s <mcd | mysql | file> <operations> <file name> \n", argv[0]);
        return -1;
    }

    if ( !strcmp(argv[2], "get") ) {
        file_get(argv[3]);
    } else if ( !strcmp(argv[2], "put") ) {
        //file_put(key);
    }

    return 0;
}

int mysql_test(int argc, char* argv[])
{
    if ( argc < 4 ) {
        printf("usage: %s <mcd | mysql | file> <operations> <file name> \n", argv[0]);
        return -1;
    }
    
    mysql_op(argv[2], argv[3]);

    return 0;
}

int mcd_test(int argc, char* argv[])
{
    if ( argc < 4 ) {
        printf("usage: %s <mcd | mysql | file> <operations> <file name> \n", argv[0]);
        return -1;
    }

    if ( !strcmp(argv[2], "put") ) {
        mcd_put(argv[3]);
    } else if ( !strcmp(argv[2], "get") ) {
        mcd_get(argv[3]);
    } else if ( !strcmp(argv[2], "check") ) {
	    mcd_check(argv[3]);
    } else if ( !strcmp(argv[2], "remove") ) {
        mcd_rm(argv[3]);
    } else if ( !strcmp(argv[2], "flush") ) {
        flush();
    } 

    return 0;
}

/*
 * Main Function
 */
int main(int argc, char *argv[]) {
    struct timeval s;

    if ( argc < 4 ) {
        printf("usage: %s <mcd | mysql | file> <operations> <file name> \n", argv[0]);
        return -1;
    }

    time_marking(&s);

    if ( !strcmp(argv[1], "mcd") ) {
        mcd_test(argc, argv);
    } else if ( !strcmp(argv[1], "mysql") ) {
        mysql_test(argc, argv);
    } else if ( !strcmp(argv[1], "file") ) {
        file_test(argc, argv);
    } 

    /*
    if ( !strcmp(argv[1], "put") ) {
        put(argv[2]);
    } else if ( !strcmp(argv[1], "get") ) {
        get(argv[2]);
    } else if ( !strcmp(argv[1], "check") ) {
	    check(argv[2]);
    } else if ( !strcmp(argv[1], "remove") ) {
        rm(argv[2]);
    } else if ( !strcmp(argv[1], "mysql") ) {
    } else if ( !strcmp(argv[1], "file") ) {
    } else if ( !strcmp(argv[1], "flush") ) {
        flush();
    }
    */

    time_diff(&s);

	return 0;
}
