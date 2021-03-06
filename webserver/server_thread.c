#include "request.h"
#include "server_thread.h"
#include "common.h"
#include <pthread.h>
#include <stdbool.h>


/* critical section */
// sv, buffer_in, buffer_out, request_queue_counter

/* data structure */

struct cache_table_element
{
    struct file_data* cach_file;
    int transmitting;
    bool deleted; 
    struct cache_table_element *next_conflict_element;
}; typedef struct cache_table_element cache_table_element;

struct cache_table
{
	int table_size;
    struct cache_table_element **hash_element;
}; typedef struct cache_table cache_table;

struct rlu_list
{
	char* cache_file_name;
	struct rlu_list *prev;
	struct rlu_list * next;
}; typedef struct rlu_list rlu_list;

struct server {
	int nr_threads;
	int max_requests;
	int max_cache_size;
	int *r_buffer;
	pthread_t **t_pool;
	cache_table* cache;

}; typedef struct server server;

struct request {
	int fd;		 /* descriptor for client connection */
	struct file_data *data;
};
/* static functions & global variables */
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t notfull = PTHREAD_COND_INITIALIZER; 
pthread_cond_t notempty = PTHREAD_COND_INITIALIZER; 
int buffer_in = 0;
int buffer_out = 0;
int request_queue_counter = 0;
int cache_size_counter = 0;
rlu_list *rlu_file_list = NULL;


/* function declaration */
void work_thread(struct server *sv);
struct cache_table_element* cache_lookup(struct server *sv, char* word);
void exist_list_updater(const struct request *rq);
struct cache_table_element* cache_insert(struct server *sv, const struct request *rq);
int cache_evict(struct server *sv, int amount_to_evict);
void new_list_updater(const struct request *rq);

/* initialize file data */
static struct file_data *
file_data_init(void)
{
	struct file_data *data;

	data = Malloc(sizeof(struct file_data));
	data->file_name = NULL;
	data->file_buf = NULL;
	data->file_size = 0;
	return data;
}

/* free all file data */
static void
file_data_free(struct file_data *data)
{

	free(data->file_name);
	free(data->file_buf);
	free(data);
}

static void
do_server_request(struct server *sv, int connfd)
{
	int ret;
	struct request *rq;
	struct file_data *data;

	data = file_data_init();

	/* fills data->file_name with name of the file being requested */
	rq = request_init(connfd, data);
	if (!rq) {
		file_data_free(data);
		return;
	}
	
	if(sv != NULL)
	{
		
		pthread_mutex_lock(&lock);
		cache_table_element* current_element = cache_lookup(sv, rq->data->file_name);
		if (current_element != NULL) //found in cache
		{	
			assert(!strcmp(rq->data->file_name, current_element->cach_file->file_name));
			rq->data->file_buf = strdup(current_element->cach_file->file_buf);
			rq->data->file_size = current_element->cach_file->file_size;
			current_element->transmitting++;
			exist_list_updater(rq);	
	    }

		else
		{
			pthread_mutex_unlock(&lock); // unlock to read disk!
			/* reads file, 
		 	* fills data->file_buf with the file contents,
	 		* data->file_size with file size. */
			ret = request_readfile(rq);
			if (!ret)
				goto out;
			pthread_mutex_lock(&lock);

			current_element = cache_lookup(sv, rq->data->file_name); // double check after reacquire the lock!
			if (current_element == NULL) // call insert
			{
				current_element = cache_insert(sv, rq);

				if(current_element != NULL)
				{
					assert(!strcmp(rq->data->file_name, current_element->cach_file->file_name));
					current_element->transmitting++;
					new_list_updater(rq);
				}
			}
			else //do not call insert, exist in cache
			{
				assert(!strcmp(rq->data->file_name, current_element->cach_file->file_name));
				rq->data->file_buf = strdup(current_element->cach_file->file_buf);
				rq->data->file_size = current_element->cach_file->file_size;
				current_element->transmitting++;
				exist_list_updater(rq);	
			}
		}	
	
		pthread_mutex_unlock(&lock); // unlock to send!
		request_sendfile(rq);
	out:
		if (current_element != NULL)
		{
			pthread_mutex_lock(&lock);
			current_element->transmitting--;
			pthread_mutex_unlock(&lock);
		}
		request_destroy(rq);
		file_data_free(data);
	}

	else //nocache
	{
		/* reads file, 
	 	* fills data->file_buf with the file contents,
	 	* data->file_size with file size. */
		ret = request_readfile(rq);
		if (!ret)
			goto out_0;
		/* sends file to client */
		request_sendfile(rq);
	out_0:
		request_destroy(rq);
		file_data_free(data);
	}
}

/* entry point functions */
void work_thread(struct server *sv)
{
	while(1)
	{
		pthread_mutex_lock(&lock);
		while(request_queue_counter == 0)
		{
			pthread_cond_wait(&notempty, &lock);
		}
		int connfd = sv->r_buffer[buffer_out];
		if (request_queue_counter == sv->max_requests)
			pthread_cond_signal(&notfull);
		buffer_out = (buffer_out+1)%(sv->max_requests);
		request_queue_counter = request_queue_counter -1;
		bool nocache = true;		
		if(sv->max_cache_size >0)
			nocache = false;
		pthread_mutex_unlock(&lock);
		if(!nocache)
			do_server_request(sv, connfd); //do reuqest after unlock, multiple threads can do this at the same time, no critical section invlved 
		else
			do_server_request(NULL, connfd);
	}
}
	
struct server * server_init(int nr_threads, int max_requests, int max_cache_size)
{
	pthread_mutex_lock(&lock);
	struct server *sv;

	sv = malloc(sizeof(struct server));
	sv->nr_threads = nr_threads;
	sv->max_requests = max_requests;
	sv->max_cache_size = max_cache_size;
	sv->r_buffer = NULL;
	sv->t_pool = NULL;
	sv->cache = NULL;
	if (nr_threads > 0 || max_requests > 0 || max_cache_size > 0) 
	{
		/* Lab 4: create queue of max_request size when max_requests > 0 */
		if(max_requests > 0)
		{
			sv->r_buffer = (int*)malloc(sizeof(int)*max_requests);
		}
		else if(max_requests <=0)
		{
			sv->r_buffer = NULL;
		}

		/* Lab 4: create worker threads when nr_threads > 0 */
		if(nr_threads > 0)
		{
			int i;
			sv->t_pool = (pthread_t**)malloc(sizeof(pthread_t*)*nr_threads);
			for(i = 0; i < nr_threads; i++)
			{
				sv->t_pool[i] = (pthread_t*)malloc(sizeof(pthread_t));
				pthread_create(sv->t_pool[i], NULL, (void*)&work_thread, sv);
			}
		}
		else if (nr_threads == 0)
		{
			sv->t_pool = NULL;
		}

		/* Lab 5: init server cache and limit its size to max_cache_size */
		if(max_cache_size > 0)
		{
			sv->cache = (cache_table*)malloc(sizeof(cache_table));
			assert(sv->cache);
			sv->cache->table_size = max_cache_size;
			sv->cache->hash_element = (cache_table_element **)malloc(sizeof(cache_table_element *)*max_cache_size);
			int i;
    		for(i=0; i< sv->cache->table_size ; i++)
    			sv->cache->hash_element[i] = NULL;
		}
		else if (max_cache_size ==0)
		{
			sv->cache = NULL;
		}
	}

	pthread_mutex_unlock(&lock);
	
	return sv;
}

void server_request(struct server *sv, int connfd)
{
	if (sv->nr_threads == 0) 
	{ 	/* no worker threads */
		do_server_request(NULL, connfd);
	} 

	else 
	{	pthread_mutex_lock(&lock);
		/*  Save the relevant info in a buffer and have one of the
		 *  worker threads do the work. */
		while (request_queue_counter == sv->max_requests)
		{
			pthread_cond_wait(&notfull, &lock);
		}
		(sv->r_buffer)[buffer_in]=connfd;	
		if(request_queue_counter == 0)
			pthread_cond_signal(&notempty);
		buffer_in = (buffer_in+1)%(sv->max_requests);
		request_queue_counter = request_queue_counter + 1;		
		pthread_mutex_unlock(&lock);
	}	
}

// Calling function of cache_lookup MUST hold the lock!!
struct cache_table_element* cache_lookup(struct server *sv, char* word )
{
	//printf("lookup\n");
	// calculate hash value
	// Original Hash Function: djb2 Original Author: dan bernstein

	int hash_value = 2*strlen(word)+1;
	int char_int;
	int char_count;
	
	for (char_count=0; char_count<strlen(word); char_count++)
	{
        char_int = (int)word[char_count];
        hash_value = hash_value * 33 + char_int;
	}
    hash_value = abs(hash_value % (sv->cache->table_size));
    if(sv->cache->hash_element[hash_value]==NULL)
       	return NULL;
    else
    { 

    	cache_table_element* current_element = sv->cache->hash_element[hash_value];
    	while (current_element != NULL)  // go throught linked list at the same hash value
        {
            if(!current_element->deleted && !strcmp(current_element->cach_file->file_name, word))  //if same word and not deleted, found         		
          		return current_element;
            else     // different word keep going              
                current_element = current_element->next_conflict_element;
        }
        return NULL;
    }
}

// Calling function of cache_insert MUST hold the lock!!
// Check double copy in calling function (call lookup) before calling inster!! (lock reacquired!)
struct cache_table_element* cache_insert(struct server *sv, const struct request *rq)
{
	//printf("insert\n");
	if(rq->data->file_size >  sv->max_cache_size)
       return NULL;

	if (cache_size_counter + rq->data->file_size > sv->max_cache_size)
	{
		int temp = cache_evict(sv, (cache_size_counter + rq->data->file_size - sv->max_cache_size)); // evict while hold the lock!
		if (temp == 0)
			return NULL;
	}

	// update size counter
	cache_size_counter = cache_size_counter + rq->data->file_size;

	// update cache
	// calculate hash value
	// Original Hash Function: djb2 Original Author: dan bernstein
	char* word = rq->data->file_name;
	int hash_value = 2*strlen(word)+1;
	int char_int;
	int char_count;
	for (char_count=0; char_count<strlen(word); char_count++)
	{
        char_int = (int)word[char_count];
        hash_value = hash_value * 33 + char_int;
	}
    hash_value = abs(hash_value % (sv->cache->table_size));    

    //set up new element
    cache_table_element* new_element = (cache_table_element*)malloc(sizeof(cache_table_element)); 
    assert(new_element);

    new_element->cach_file = file_data_init();

    new_element->cach_file->file_name= strdup(rq->data->file_name);
    
    new_element->cach_file->file_buf = strdup(rq->data->file_buf);
    new_element->cach_file->file_size = rq->data->file_size;

    new_element->transmitting = 0;
    new_element->deleted = false;
	new_element->next_conflict_element = NULL;
 
	if (sv->cache->hash_element[hash_value] == NULL) // empty, direct insert
	{
		sv->cache->hash_element[hash_value] = new_element;
		return new_element;
	}
    
    else
    {
        cache_table_element* current_element = sv->cache->hash_element[hash_value];
        cache_table_element* previous_element = NULL;
        while (current_element!=NULL) 
        {
            if(current_element->deleted == true) 
            {
                new_element->next_conflict_element = current_element->next_conflict_element;
                if(previous_element == NULL)
                	sv->cache->hash_element[hash_value] = new_element;
                else
                	previous_element->next_conflict_element = new_element;
               free(current_element);
                return new_element;
            }
            else                                   
            {
                previous_element = current_element;
                current_element = current_element->next_conflict_element;
            }
        }
        //no space, add new conflict element to end of the list
        previous_element->next_conflict_element = new_element;
        return new_element;
    }
}

int cache_evict(struct server *sv, int amount_to_evict)
{
	//printf("evict\n");
	bool no_more = false;
	if(rlu_file_list == NULL)
        assert(0);
    else
    {	//int i =0;
    	//while(i<10000)
    	//	i++;
    	rlu_list * current_node = rlu_file_list;
    	rlu_list * last_node = NULL;
    	while(current_node->next != NULL)
        {
            current_node = current_node->next;
        }
        last_node = current_node;

    	while(amount_to_evict > 0 && !no_more)
    	{
        	cache_table_element* current_element = cache_lookup(sv, last_node->cache_file_name);
        	while (!no_more && current_element->transmitting > 0)
        	{
        		last_node = last_node->prev;
           		if(last_node != NULL)
        			current_element = cache_lookup(sv, last_node->cache_file_name);
        		else
        			no_more = true;
        	}
        	
        	if (!no_more)
        	{
        		amount_to_evict = amount_to_evict- current_element->cach_file->file_size;
        		cache_size_counter = cache_size_counter - current_element->cach_file->file_size;
        		if(last_node->prev != NULL)
				{
					last_node->prev->next = last_node->next;
					if(last_node->next!=NULL)
						last_node->next->prev = last_node->prev;
				}
				else
				{
					rlu_file_list=last_node->next;
					no_more=true;
					if(last_node->next!=NULL)
						last_node->next->prev=NULL;
				}
				rlu_list *temp = last_node;
				last_node = last_node->prev;
        		free(temp);
        		current_element->deleted = true;
        		file_data_free(current_element->cach_file);
        		current_element->cach_file = NULL;
        	}
    	}
    	
    }
	if(no_more)
    	return 0;
    else
    	return 1;
}

void exist_list_updater(const struct request *rq)
{
	//printf("update1\n");
	//update exist rlu list
	rlu_list * current_node = rlu_file_list;
	//searching..
    while(current_node!= NULL)
    {
        if (!strcmp(current_node->cache_file_name, rq->data->file_name))
        {
            if(current_node->prev != NULL)
            {
            	current_node->prev->next = current_node->next;
            	if(current_node->next!=NULL)
            		current_node->next->prev = current_node->prev;
				current_node->next = rlu_file_list;
				rlu_file_list->prev = current_node;
				rlu_file_list = current_node;
				current_node->prev = NULL;
            }
            return;
        }
        else
        {		
            current_node = current_node->next;
        }
    }
    assert(0);
} 

void new_list_updater(const struct request *rq)
{
	//printf("update2\n");
	if(rlu_file_list==NULL)
	{
		rlu_file_list = (rlu_list*)malloc(sizeof(rlu_list));
		assert(rlu_file_list);
		rlu_file_list->cache_file_name = strdup(rq->data->file_name);
		assert(rlu_file_list->cache_file_name );
		rlu_file_list->next = NULL;
		rlu_file_list->prev = NULL;
	}
	else
	{
		rlu_list* new_node = (rlu_list*)malloc(sizeof(rlu_list));
		assert(new_node);
		new_node->cache_file_name = strdup(rq->data->file_name);
		assert(new_node->cache_file_name );
		rlu_file_list->prev = new_node;
		new_node->next = rlu_file_list;
		new_node->prev = NULL;
		rlu_file_list = new_node;

	}
}
