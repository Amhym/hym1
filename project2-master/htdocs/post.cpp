#include <iostream>
#include <time.h>
#include <string.h>

using namespace std;

char from_hex(char ch) {
	return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

int main()
{
    char ch;
    char *p = getenv("CONTENT_LENGTH");/*从环境变量CONTENT_LENGTH中得到数据长度*/
    int len = atoi(p);
    int i=0;
    setvbuf(stdin,NULL,_IONBF,0);  /*关闭stdin的缓冲*/
    cout<<"Content-Type:text/html; charset=utf-8\r\n\r\n";
    cout<<"这就是post表单的内容"<<endl;
    string s;
    while(i<len){
      ch = fgetc(stdin);
      s+=ch;
      i++;
    }
    char *chs=(char*)malloc((strlen(s.c_str())+1) * sizeof(char));;
    // cout<<s<<"====";
    strcat(chs,s.c_str());
    // cout<<chs<<"======";
    if (strstr(chs, "%")) {
			/*
			 * Convert from URL encoded string.
			 */
			char * buf = (char*)malloc(strlen(chs) + 1);
			char * pstr = chs;
			char * pbuf = buf;
			while (*pstr) {
				if (*pstr == '%') {
					if (pstr[1] && pstr[2]) {
						*pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
						pstr += 2;
					}
				} else if (*pstr == '+') { 
					*pbuf++ = ' ';
				} else {
					*pbuf++ = *pstr;
				}
				pstr++;
			}
			*pbuf = '\0';
			free(chs);
			chs = buf;
		}
        cout<<chs;
}