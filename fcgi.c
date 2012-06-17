#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define u8 unsigned char
#define u16 unsigned short
#define u32 unsigned long

#include <stdarg.h>

void nlog(char *str, ...)
{
	va_list ap;
	char buf[256];
	static FILE *f;
	if(!f)
		f = fopen("/tmp/blah.txt", "wb");

	va_start(ap, str);
	vsprintf(buf, str, ap);
	va_end(ap);

	fprintf(f, "%s", buf);
	fflush(f);
}

//#define nlog(x, ...){}

enum {
	BEGIN_REQUEST = 1,
	ABORT_REQUEST,
	END_REQUEST,
	PARAMS,
	STDIN,
	STDOUT,
	STDERR,
	DATA,
	GET_VALUES,
	GET_VALUES_RESULT,
	UNKNOWN_TYPE
};

struct env_record {
	u8 *name;
	u8 *value;
};

struct env {
	u32 num_records, max_records;
	struct env_record *record;
};

struct fcgi_record {
	u8 version, type, padl, resv;
	u16 cl, id;
	u8 *data;
	u8 *pad;
};

struct session {
	u16 id;
	u8 role, flags;
	struct env env;
	struct fcgi_record r;
	int active;
};

static struct session sessions[32];

static u16 get_record(int sock)
{
	u8 buf[8];
	u16 id;

	read(sock, buf, 8);

	id = (buf[2] << 8) | buf[3];

	sessions[id].active = 1;

	sessions[id].r.version = buf[0];
	sessions[id].r.type    = buf[1];
	sessions[id].r.id      = id;
	sessions[id].r.cl      = (buf[4]<<8) | buf[5];
	sessions[id].r.padl    = buf[6];
	sessions[id].r.resv    = buf[7];

	return id;
}

static void env_add_record(struct session *s, u8 *name, u32 namelen, u8 *value, u32 valuelen)
{
	struct env_record *er;

	if(s->env.max_records == 0)
	{
		s->env.max_records = 32;
		s->env.record = malloc(sizeof(struct env_record) * 32);
	}

	if(s->env.max_records == s->env.num_records)
	{
		s->env.max_records *= 2;
		s->env.record = realloc(s->env.record, sizeof(struct env_record) * s->env.max_records);	
	}

	er = &s->env.record[s->env.num_records];

	er->name = malloc(namelen + 1);
	memcpy(er->name, name, namelen);
	er->name[namelen] = '\0';

	er->value = malloc(valuelen +1);
	memcpy(er->value, value, valuelen);
	er->value[valuelen] = '\0';
	s->env.num_records++;
}

static void read_pairs(int sock, struct session *s)
{
	u32 namelen;
	u32 valuelen;
	u8 *name, *value;
	u8 *content;
	u8 *stream;

	content = malloc(s->r.cl);
	read(sock, content, s->r.cl);

	stream = content;

	while(stream < content + s->r.cl)
	{
		namelen = *stream++;

		/* Hibit set converts this into a 4 byte integer, rather than 7 bit */
		if(namelen & 0x80)
		{
			namelen = (namelen & 0x7F)<<8;;
			namelen = (namelen | stream[0])<<8;
			namelen = (namelen | stream[1])<<8;
			namelen = (namelen | stream[2]);
			stream += 3;
		}

		valuelen = *stream++;

		if(valuelen & 0x80)
		{
			valuelen = (valuelen & 0x7F)<<8;;
			valuelen = (valuelen | stream[0])<<8;
			valuelen = (valuelen | stream[1])<<8;
			valuelen = (valuelen | stream[2]);
			stream += 3;
		}

		name = stream;
		stream += namelen;

		value = stream;
		stream += valuelen;
		env_add_record(s, name, namelen, value, valuelen);
	}

	free(content);
}

static void destroy_session(struct session *s)
{
	int i;

	for(i = 0; i < s->env.num_records; i++)
	{
		free(s->env.record[i].name);
		free(s->env.record[i].value);
	}

	s->active = 0;

	free(s->env.record);
}


static void begin_request(int sock, struct session *s)
{
	u8 buf[8];

	read(s, buf, 8);

	s->role  = (buf[0]<<8) | buf[1]; 	
	s->flags = buf[2];
}

/* First we get a record over the socket which contains
 * the length of the payload and the type of it.
 * Depending on the type, different functions will be
 * called to read and interperet the incoming payload.
 */
static int do_request(int sock)
{
	u16 id;

	id = get_record(sock);

	switch(sessions[id].r.type)
	{
		case BEGIN_REQUEST:
			/* Tells us which role we are, and a keep-alive flag */
			begin_request(sock, &sessions[id]);
		break;
		case PARAMS:
			read_pairs(sock, &sessions[id]);
		break;
		case STDIN:
			return id;
		break;
		default:
			exit(0);
		break;
	}

	return 0;
}

static int sock;

static void stdout_start(int sock, int id, int len)
{
	u8 buf[8];

	buf[0] = 1;
	buf[1] = STDOUT;
	buf[2] = id>>8;
	buf[3] = id & 0xFF;
	buf[4] = 0;
	buf[5] = len;
	buf[6] = 0;
	buf[7] = 0;

	write(sock, buf, 8); /* FCGI_STDOUT start*/
}

static void stdout_finish(int sock, int id)
{
	u8 buf[8];

	buf[0] = 1;
	buf[1] = STDOUT;
	buf[2] = id>>8;
	buf[3] = id & 0xFF;
	buf[4] = 0;
	buf[5] = 0;
	buf[6] = 0;
	buf[7] = 0;
	write(sock, buf, 8);	/* FCGI_STDOUT finish */
}

static void request_complete(int sock, int id)
{
	u8 buf[8];

	buf[0] = 1;
	buf[1] = END_REQUEST;
	buf[2] = id>>8;
	buf[3] = id&0xFF;
	buf[4] = 0;
	buf[5] = 8;
	buf[6] = 0;
	buf[7] = 0;
	write(sock, buf, 8);

	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	buf[4] = 0; /* FCGI_REQUEST_COMPLETE */
	buf[5] = 0;
	buf[6] = 0;
	buf[7] = 0;
	write(sock, buf, 8);
}

static void stdout_send(int sock, int id, const char *msg, int len)
{
	stdout_start(sock, id, len);
	
	write(sock, msg, len);
}

void fcgi_close(int id)
{
	stdout_finish(sock, id);
	request_complete(sock, id);
}

void fcgi_send(int id, const char *msg, int len)
{
	stdout_send(sock, id, msg, len);
}

int fcgi_accept(void)
{
	struct sockaddr sa;
	socklen_t sa_len;
	int id;

	sock = accept(0, &sa, &sa_len);

	/* Keep processing header stuff until we get to FCGI_STDIN
	 * so we can send the client data.
	 */
	while(1)
	{
		id = do_request(sock);
		if(id)
			break;
	}

	return id;
}

