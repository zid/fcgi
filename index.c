#include "fcgi.h"

int main(void)
{
	int id;
	char msg[] = "Content-type: text/html; charset=utf-8\r\n\r\nHello, World!\n";

	while(1)
	{
		id = fcgi_accept();
	
		fcgi_send(id, msg, sizeof(msg)-1);

		fcgi_close(id);
	}
}
