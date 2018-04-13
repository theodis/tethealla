/************************************************************************
  Tethealla Patch Server
  Copyright (C) 2008  Terry Chatman Jr.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License version 3 as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
************************************************************************/

#include  <windows.h>
#include  <stdio.h>
#include  <string.h>
#include  <time.h>
#include  "resource.h"

// Encryption data struct
typedef struct {
    unsigned long keys[1042]; // encryption stream
    unsigned long pc_posn; // PSOPC crypt position
} CRYPT_SETUP;

unsigned long CRYPT_PC_GetNextKey(CRYPT_SETUP*);
void CRYPT_PC_MixKeys(CRYPT_SETUP*);
void CRYPT_PC_CreateKeys(CRYPT_SETUP*,unsigned long);
void CRYPT_PC_CryptData(CRYPT_SETUP*,void*,unsigned long);

#define MAX_PATCHES 4096
#define PATCH_COMPILED_MAX_CONNECTIONS 300
#define SERVER_VERSION "0.010"

#define SEND_PACKET_02 0x00
#define RECEIVE_PACKET_02 0x01
#define RECEIVE_PACKET_10 0x02
#define SEND_PACKET_0B 0x03
#define MAX_SENDCHECK 0x04

//#define USEADDR_ANY
//#define DEBUG_OUTPUT
#define TCP_BUFFER_SIZE 65530

struct timeval select_timeout = {
  0,
  5000
};

/* functions */

void send_to_server(int sock, char* packet);
int receive_from_server(int sock, char* packet);
void debug(char *fmt, ...);
void debug_perror(char * msg);
void tcp_listen (int sockfd);
int tcp_accept (int sockfd, struct sockaddr *client_addr, int *addr_len );
int tcp_sock_connect(char* dest_addr, int port);
int tcp_sock_open(struct in_addr ip, int port);

/* "Welcome" Packet */

unsigned char Packet02[] = {
  0x4C, 0x00, 0x02, 0x00, 0x50, 0x61, 0x74, 0x63, 0x68, 0x20, 0x53, 0x65, 0x72, 0x76, 0x65, 0x72,
  0x2E, 0x20, 0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x53, 0x6F, 0x6E, 0x69,
  0x63, 0x54, 0x65, 0x61, 0x6D, 0x2C, 0x20, 0x4C, 0x54, 0x44, 0x2E, 0x20, 0x32, 0x30, 0x30, 0x31,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x2D, 0x69, 0x06, 0x9E, 0xDC, 0xE0, 0x6F, 0xCA
};

const unsigned char Message02[] = { "Tethealla Patch" };

/* String sent to server to retrieve IP address. */

char* HTTP_REQ = "GET http://www.pioneer2.net/remote.php HTTP/1.0\r\n\r\n\r\n";

/* Populated by load_config_file(): */

unsigned char serverIP[4];
unsigned short serverPort;
int override_on = 0;
unsigned char overrideIP[4];
unsigned short serverMaxConnections;
unsigned serverNumConnections = 0;
unsigned serverConnectionList[PATCH_COMPILED_MAX_CONNECTIONS]; // One patch, one data.

char Welcome_Message[4096] = {0};
unsigned short Welcome_Message_Size = 0;
time_t servertime;
time_t sendtime;
int maxbytes  = 0;

/* Client Structure */

typedef struct st_patch_data {
  unsigned file_size;
  unsigned checksum;
  char full_file_name[MAX_PATH+48];
  char file_name[48];
  char folder[MAX_PATH];
  unsigned char patch_folders[128]; // Command to get to the folder this file resides in...
  unsigned patch_folders_size;
  unsigned patch_steps; // How many steps from the root folder this file is...
} patch_data;

typedef struct st_client_data {
  unsigned file_size;
  unsigned checksum;
} client_data;

typedef struct st_banana {
  int patch;
  int plySockfd;
  unsigned char peekbuf[8];
  unsigned char rcvbuf [TCP_BUFFER_SIZE];
  unsigned short rcvread;
  unsigned short expect;
  unsigned char decryptbuf [TCP_BUFFER_SIZE];
  unsigned char sndbuf [TCP_BUFFER_SIZE];
  unsigned char encryptbuf [TCP_BUFFER_SIZE];
  int snddata,
    sndwritten;
  unsigned char packet [TCP_BUFFER_SIZE];
  unsigned short packetdata;
  unsigned short packetread;
  int crypt_on;
  CRYPT_SETUP server_cipher, client_cipher;
  client_data p_data[MAX_PATCHES];
  int sending_files;
  unsigned files_to_send;
  unsigned bytes_to_send;
  unsigned s_data[MAX_PATCHES];
  char username[17];
  unsigned current_file;
  unsigned cfile_index;
  unsigned lastTick;    // The last second
  unsigned toBytesSec;  // How many bytes per second the server sends to the client
  unsigned fromBytesSec;  // How many bytes per second the server receives from the client
  unsigned packetsSec;  // How many packets per second the server receives from the client
  unsigned connected;
  unsigned char sendCheck[MAX_SENDCHECK+2];
  int todc;
  char patch_folder[MAX_PATH];
  unsigned patch_steps;
  unsigned chunk;
  unsigned char IP_Address[16];
  unsigned connection_index;
} BANANA;

fd_set ReadFDs, WriteFDs, ExceptFDs;

#define MAX_SIMULTANEOUS_CONNECTIONS 6

unsigned char dp[TCP_BUFFER_SIZE*4];
char PacketData[TCP_BUFFER_SIZE];

unsigned char patch_packet[TCP_BUFFER_SIZE];
unsigned patch_size = 0;
patch_data s_data[MAX_PATCHES];
unsigned serverNumPatches = 0;

void decryptcopy ( void* dest, void* source, unsigned size );
void encryptcopy ( BANANA* client, void* source, unsigned size );

CRYPT_SETUP *cipher_ptr;

#define MYWM_NOTIFYICON (WM_USER+2)
int program_hidden = 1;
HWND consoleHwnd;


void display_packet ( unsigned char* buf, int len )
{
  int c, c2, c3, c4;

  c = c2 = c3 = c4 = 0;

  for (c=0;c<len;c++)
  {
    if (c3==16)
    {
      for (;c4<c;c4++)
        if (buf[c4] >= 0x20)
          dp[c2++] = buf[c4];
        else
          dp[c2++] = 0x2E;
      c3 = 0;
      sprintf (&dp[c2++], "\n" );
    }

    if ((c == 0) || !(c % 16))
    {
      sprintf (&dp[c2], "(%04X) ", c);
      c2 += 7;
    }

    sprintf (&dp[c2], "%02X ", buf[c]);
    c2 += 3;
    c3++;
  }

  if ( len % 16 )
  {
    c3 = len;
    while (c3 % 16)
    {
      sprintf (&dp[c2], "   ");
      c2 += 3;
      c3++;
    }
  }

  for (;c4<c;c4++)
    if (buf[c4] >= 0x20)
      dp[c2++] = buf[c4];
    else
      dp[c2++] = 0x2E;

  dp[c2] = 0;
  printf ("%s\n\n", &dp[0]);
}

void convertIPString (char* IPData, unsigned IPLen, int fromConfig )
{
  unsigned p,p2,p3;
  char convert_buffer[5];

  p2 = 0;
  p3 = 0;
  for (p=0;p<IPLen;p++)
  {
    if ((IPData[p] > 0x20) && (IPData[p] != 46))
      convert_buffer[p3++] = IPData[p]; else
    {
      convert_buffer[p3] = 0;
      if (IPData[p] == 46) // .
      {
        serverIP[p2] = atoi (&convert_buffer[0]);
        p2++;
        p3 = 0;
        if (p2>3)
        {
          if (fromConfig)
            printf ("tethealla.ini is corrupted. (Failed to read IP information from file!)\n"); else
            printf ("Failed to determine IP address.\n");
          exit (1);
        }
      }
      else
      {
        serverIP[p2] = atoi (&convert_buffer[0]);
        if (p2 != 3)
        {
          if (fromConfig)
            printf ("tethealla.ini is corrupted. (Failed to read IP information from file!)\n"); else
            printf ("Failed to determine IP address.\n");
          exit (1);
        }
        break;
      }
    }
  }
}

long CalculateChecksum(void* data,unsigned long size)
{
    long offset,y,cs = 0xFFFFFFFF;
    for (offset = 0; offset < (long)size; offset++)
    {
        cs ^= *(unsigned char*)((long)data + offset);
        for (y = 0; y < 8; y++)
        {
            if (!(cs & 1)) cs = (cs >> 1) & 0x7FFFFFFF;
            else cs = ((cs >> 1) & 0x7FFFFFFF) ^ 0xEDB88320;
        }
    }
    return (cs ^ 0xFFFFFFFF);
}


void load_config_file()
{
  int config_index = 0;
  char config_data[255];
  unsigned ch;

  FILE* fp;

  if ( ( fp = fopen ("tethealla.ini", "r" ) ) == NULL )
  {
    printf ("\nThe configuration file tethealla.ini appears to be missing.\n");
    exit (1);
  }
  else
    while (fgets (&config_data[0], 255, fp) != NULL)
    {
      if (config_data[0] != 0x23)
      {
        if ((config_index < 0x04) || (config_index > 0x04))
        {
          ch = strlen (&config_data[0]);
          if (config_data[ch-1] == 0x0A)
            config_data[ch--]  = 0x00;
          config_data[ch] = 0;
        }
        switch (config_index)
        {
        case 0x00:
          // MySQL Host
          //memcpy (&mySQL_Host[0], &config_data[0], ch+1);
          break;
        case 0x01:
          // MySQL Username
          //memcpy (&mySQL_Username[0], &config_data[0], ch+1);
          break;
        case 0x02:
          // MySQL Password
          //memcpy (&mySQL_Password[0], &config_data[0], ch+1);
          break;
        case 0x03:
          // MySQL Database
          //memcpy (&mySQL_Database[0], &config_data[0], ch+1);
          break;
        case 0x04:
          // MySQL Port
          //mySQL_Port = atoi (&config_data[0]);
          break;
        case 0x05:
          // Server IP address
          {
            if ((config_data[0] == 0x41) || (config_data[0] == 0x61))
            {
              struct sockaddr_in pn_in;
              struct hostent *pn_host;
              int pn_sockfd, pn_len;
              char pn_buf[512];
              char* pn_ipdata;

              printf ("\n** Determining IP address ... ");

              pn_host = gethostbyname ( "www.pioneer2.net" );
              if (!pn_host) {
                printf ("Could not resolve www.pioneer2.net\n");
                exit (1);
              }

              /* Create a reliable, stream socket using TCP */
              if ((pn_sockfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
              {
                printf ("Unable to create TCP/IP streaming socket.");
                exit(1);
              }

              /* Construct the server address structure */
              memset(&pn_in, 0, sizeof(pn_in)); /* Zero out structure */
              pn_in.sin_family = AF_INET; /* Internet address family */

              memcpy(&pn_in.sin_addr.s_addr, pn_host->h_addr, 4); /* Web Server IP address */

              pn_in.sin_port = htons(80); /* Web Server port */

              /* Establish the connection to the pioneer2.net Web Server ... */

              if (connect(pn_sockfd, (struct sockaddr *) &pn_in, sizeof(pn_in)) < 0)
              {
                printf ("\nCannot connect to www.pioneer2.net!");
                exit(1);
              }

              /* Process pioneer2.net's response into the serverIP variable. */

              send_to_server ( pn_sockfd, HTTP_REQ );
              pn_len = recv(pn_sockfd, &pn_buf[0], sizeof(pn_buf) - 1, 0);
              closesocket (pn_sockfd);
              pn_buf[pn_len] = 0;
              pn_ipdata = strstr (&pn_buf[0], "/html");
              if (!pn_ipdata)
              {
                printf ("Failed to determine IP address.\n");
              }
              else
                pn_ipdata += 9;

              convertIPString (pn_ipdata, strlen (pn_ipdata), 0 );
            }
            else
            {
              convertIPString (&config_data[0], ch+1, 1);
            }
          }
          break;
        case 0x06:
          // Welcome Message
          break;
        case 0x07:
          // Server Listen Port
          serverPort = atoi (&config_data[0]);
          break;
        case 0x08:
          // Max Client Connections
          serverMaxConnections = atoi (&config_data[0]);
          if ( serverMaxConnections > PATCH_COMPILED_MAX_CONNECTIONS )
          {
             serverMaxConnections = PATCH_COMPILED_MAX_CONNECTIONS;
             printf ("This version of the patch server has not been compiled to handle more than %u patch connections.  Adjusted.\n", PATCH_COMPILED_MAX_CONNECTIONS );
          }
          if ( !serverMaxConnections )
            serverMaxConnections  = PATCH_COMPILED_MAX_CONNECTIONS;
          break;
        case 0x09:
          break;
        case 0x0A:
          // Override IP address (if specified, this IP will be sent out instead of your own to those who connect)
          if ((config_data[0] > 0x30) && (config_data[0] < 0x3A))
          {
            override_on = 1;
            memcpy (&overrideIP[0], &serverIP[0], 4);
            serverIP[0] = 0;
            convertIPString (&config_data[0], ch+1, 1);
          }
          break;
        default:
          break;
        }
        config_index++;
      }
    }
    fclose (fp);

  if (config_index < 0x13)
  {
    printf ("tethealla.ini seems to be corrupted.\n");
    exit (1);
  }

  printf ("  OK!\n");
  printf ("Loading configuration from patch.ini ...");

  // Load patch.ini here
  // Upload throttle

  config_index = 0;

  if ( ( fp = fopen ("patch.ini", "r" ) ) == NULL )
  {
    printf ("\nThe configuration file patch.ini appears to be missing.\n");
    exit (1);
  }
  else
    while (fgets (&config_data[0], 255, fp) != NULL)
    {
      if (config_data[0] != 0x23)
      {
          ch = strlen (&config_data[0]);
          if (config_data[ch-1] == 0x0A)
            config_data[ch--]  = 0x00;
          config_data[ch] = 0;
          if (config_index == 0)
          {
            maxbytes = atoi (&config_data[0]);
            if (maxbytes)
              maxbytes *= 1024;
          }
          config_index++;
      }
    }
  fclose (fp);
}

BANANA * connections[PATCH_COMPILED_MAX_CONNECTIONS];
BANANA * workConnect;

const char serverName[] = { "T\0E\0T\0H\0E\0A\0L\0L\0A\0" };


unsigned free_connection()
{
  unsigned fc;
  BANANA* wc;

  for (fc=0;fc<serverMaxConnections;fc++)
  {
    wc = connections[fc];
    if (wc->plySockfd<0)
      return fc;
  }
  return 0xFFFF;
}


void initialize_connection (BANANA* connect)
{
  unsigned ch, ch2;

  if (connect->plySockfd >= 0)
  {
    ch2 = 0;
    for (ch=0;ch<serverNumConnections;ch++)
    {
      if (serverConnectionList[ch] != connect->connection_index)
        serverConnectionList[ch2++] = serverConnectionList[ch];
    }
    serverNumConnections = ch2;
    closesocket (connect->plySockfd);
  }
  memset (connect, 0, sizeof (BANANA));
  connect->plySockfd = -1;
  connect->lastTick = 0xFFFFFFFF;
  connect->connected = 0xFFFFFFFF;
}


void start_encryption(BANANA* connect)
{
  unsigned c, c3, c4, connectNum;
  BANANA *workConnect, *c5;

  // Limit the number of connections from an IP address to MAX_SIMULTANEOUS_CONNECTIONS.

  c3 = 0;

  for (c=0;c<serverNumConnections;c++)
  {
    connectNum = serverConnectionList[c];
    workConnect = connections[connectNum];
    //debug ("%s comparing to %s", (char*) &workConnect->IP_Address[0], (char*) &connect->IP_Address[0]);
    if ((!strcmp(&workConnect->IP_Address[0], &connect->IP_Address[0])) &&
      (workConnect->plySockfd >= 0))
      c3++;
  }

  if (c3 > MAX_SIMULTANEOUS_CONNECTIONS)
  {
    // More than MAX_SIMULTANEOUS_CONNECTIONS connections from a certain IP address...
    // Delete oldest connection to server.
    c4 = 0xFFFFFFFF;
    c5 = NULL;
    for (c=0;c<serverNumConnections;c++)
    {
      connectNum = serverConnectionList[c];
      workConnect = connections[connectNum];
      if ((!strcmp(&workConnect->IP_Address[0], &connect->IP_Address[0])) &&
        (workConnect->plySockfd >= 0))
      {
        if (workConnect->connected < c4)
        {
          c4 = workConnect->connected;
          c5 = workConnect;
        }
      }
    }
    if (c5)
    {
      workConnect = c5;
      initialize_connection (workConnect);
    }
  }
  memcpy (&connect->sndbuf[0], &Packet02[0], sizeof (Packet02));
  for (c=0;c<8;c++)
    connect->sndbuf[0x44+c] = (unsigned char) rand() % 255;
  connect->snddata += sizeof (Packet02);

  memcpy (&c, &connect->sndbuf[0x44], 4);
  CRYPT_PC_CreateKeys(&connect->server_cipher,c);
  memcpy (&c, &connect->sndbuf[0x48], 4);
  CRYPT_PC_CreateKeys(&connect->client_cipher,c);
  connect->crypt_on = 1;
  connect->sendCheck[SEND_PACKET_02] = 1;
  connect->connected = (unsigned) servertime;
}

void change_client_folder (unsigned patchNum, BANANA* client);

void Send11 (BANANA* client)
{
  unsigned ch;

  client->sendCheck[RECEIVE_PACKET_10] = 1;

  for (ch=0;ch<serverNumPatches;ch++)
  {
    if ((client->p_data[ch].file_size != s_data[ch].file_size) ||
      (client->p_data[ch].checksum  != s_data[ch].checksum))
    {
      //debug ("%s mismatch", s_data[ch].file_name);
      client->s_data[client->files_to_send++] = ch;
      client->bytes_to_send += s_data[ch].file_size;
    }
  }

  if (client->files_to_send)
  {
    memset (&client->encryptbuf[0x00], 0, 0x0C);
    memcpy (&client->encryptbuf[0x04], &client->bytes_to_send, 4);
    memcpy (&client->encryptbuf[0x08], &client->files_to_send, 4);
    client->encryptbuf[0x00] = 0x0C;
    client->encryptbuf[0x02] = 0x11;
    cipher_ptr = &client->server_cipher;
    encryptcopy (client, &client->encryptbuf[0x00], 0x0C);
    change_client_folder (client->s_data[0], client);
    client->sending_files = 1; // We're in send mode!
    printf ("(%s) Sending %u files, total bytes: %u\n", client->username, client->files_to_send, client->bytes_to_send);
  }
  else
  {
    workConnect->encryptbuf[0x00] = 0x04;
    workConnect->encryptbuf[0x01] = 0x00;
    workConnect->encryptbuf[0x02] = 0x12;
    workConnect->encryptbuf[0x03] = 0x00;
    cipher_ptr = &workConnect->server_cipher;
    encryptcopy (workConnect, &workConnect->encryptbuf[0x00], 4);
  }
}

void Send0B (BANANA* client)
{
  if (!client->sendCheck[SEND_PACKET_0B])
  {
    client->sendCheck[SEND_PACKET_0B] = 1;
    client->encryptbuf[0x00] = 0x04;
    client->encryptbuf[0x01] = 0x00;
    client->encryptbuf[0x02] = 0x0B;
    client->encryptbuf[0x03] = 0x00;
    cipher_ptr = &client->server_cipher;
    encryptcopy (client, &client->encryptbuf[0x00], 4);
    cipher_ptr = &client->server_cipher;
    encryptcopy (client, &patch_packet[0], patch_size);
  }
}

void Send13 (BANANA* client)
{
  unsigned short Welcome_Size;
  unsigned char port[2];

  Welcome_Size = Welcome_Message_Size + 4;
  memcpy (&client->encryptbuf[0x04], &Welcome_Message[0], Welcome_Message_Size);
  client->encryptbuf[0x02] = 0x13;
  client->encryptbuf[0x03] = 0x00;
  client->encryptbuf[Welcome_Size++] = 0x00;
  client->encryptbuf[Welcome_Size++] = 0x00;
  while (Welcome_Size % 4)
    client->encryptbuf[Welcome_Size++] = 0x00;
  memcpy (&client->encryptbuf[0x00], &Welcome_Size, 2);
  cipher_ptr = &client->server_cipher;
  encryptcopy (client, &client->encryptbuf[0x00], Welcome_Size);
  memset (&client->encryptbuf[0x00], 0, 0x0C);
  client->encryptbuf[0x00] = 0x0C;
  client->encryptbuf[0x02] = 0x14;
  memcpy (&client->encryptbuf[0x04], &serverIP, 4);
  Welcome_Size = serverPort;
  Welcome_Size -= 999;
  memcpy (&port, &Welcome_Size, 2);
  client->encryptbuf[0x08] = port[1];
  client->encryptbuf[0x09] = port[0];
  cipher_ptr = &client->server_cipher;
  encryptcopy (client, &client->encryptbuf[0x00], 0x0C);
}

void DataProcessPacket (BANANA* client)
{
  unsigned patch_index;

  switch (client->decryptbuf[0x02])
  {
  case 0x02:
    // Acknowledging welcome packet
    client->encryptbuf[0x00] = 0x04;
    client->encryptbuf[0x01] = 0x00;
    client->encryptbuf[0x02] = 0x04;
    client->encryptbuf[0x03] = 0x00;
    cipher_ptr = &client->server_cipher;
    encryptcopy (client, &client->encryptbuf[0x00], 4);
    break;
  case 0x04:
    // Client sending user name to begin downloading patch data
    memcpy (&client->username[0], &client->decryptbuf[0x10], 16);
    Send0B (client); // Data time...
    break;
  case 0x0F:
    // Client sending status of current patch file
    memcpy (&patch_index, &client->decryptbuf[0x04], 4);
    if (patch_index < MAX_PATCHES)
    {
      memcpy (&client->p_data[patch_index].checksum,  &client->decryptbuf[0x08], 4);
      memcpy (&client->p_data[patch_index].file_size, &client->decryptbuf[0x0C], 4);
    }
    break;
  case 0x10:
    // Client done sending all patch file status
    if (!client->sendCheck[RECEIVE_PACKET_10])
      Send11 (client);
    break;
  }
}

void PatchProcessPacket (BANANA* client)
{
  switch (client->decryptbuf[0x02])
  {
  case 0x02:
    // Acknowledging welcome packet
    client->encryptbuf[0x00] = 0x04;
    client->encryptbuf[0x01] = 0x00;
    client->encryptbuf[0x02] = 0x04;
    client->encryptbuf[0x03] = 0x00;
    cipher_ptr = &client->server_cipher;
    encryptcopy (client, &client->encryptbuf[0x00], 4);
    break;
  case 0x04:
    // Client sending user name to begin downloading patch data
    memcpy (&client->username[0], &client->decryptbuf[0x10], 16);
    Send13 (client); // Welcome packet
    break;
  default:
    break;
  }
}

char patch_folder[MAX_PATH] = {0};
unsigned patch_steps = 0;
int now_folder = -1;

int scandir(char *path, BOOL recursive);
int fixpath(char *inpath, char *outpath);

void change_client_folder (unsigned patchNum, BANANA* client)
{
  unsigned ch, ch2, ch3;

  if (strcmp(&client->patch_folder[0], &s_data[patchNum].folder[0]) != 0)
  {
    // Client not in the right folder...

    while (client->patch_steps)
    {
      client->encryptbuf[0x00] = 0x04;
      client->encryptbuf[0x01] = 0x00;
      client->encryptbuf[0x02] = 0x0A;
      client->encryptbuf[0x03] = 0x00;
      cipher_ptr = &client->server_cipher;
      encryptcopy (client, &client->encryptbuf[0x00], 4);
      client->patch_steps--;
    }

    if (s_data[patchNum].patch_folders[0] != 0x2E)
    {
      ch = 0;
      while (ch<s_data[patchNum].patch_folders_size)
      {
        memset (&client->encryptbuf[0x00], 0, 0x44);
        client->encryptbuf[0x00] = 0x44;
        client->encryptbuf[0x02] = 0x09;
        ch3 = 0;
        for (ch2=ch;ch2<s_data[patchNum].patch_folders_size;ch2++)
        {
          if (s_data[patchNum].patch_folders[ch2] == 0x00)
            break;
          ch3++;
        }
        strcat (&client->encryptbuf[0x04], &s_data[patchNum].patch_folders[ch]);
        ch += (ch3 + 1);
        cipher_ptr = &client->server_cipher;
        encryptcopy (client, &client->encryptbuf[0x00], 0x44);
      }
    }
    memcpy (&client->patch_folder[0], &s_data[patchNum].folder, MAX_PATH);
    client->patch_steps = s_data[patchNum].patch_steps;
  }
  // Now let's send the information about the file coming in...
  memset (&client->encryptbuf[0x00], 0, 0x3C);
  client->encryptbuf[0x00] = 0x3C;
  client->encryptbuf[0x02] = 0x06;
  memcpy (&client->encryptbuf[0x08], &s_data[patchNum].file_size, 4);
  strcat (&client->encryptbuf[0x0C], &s_data[patchNum].file_name[0]);
  cipher_ptr = &client->server_cipher;
  encryptcopy (client, &client->encryptbuf[0x00], 0x3C);
}

void change_patch_folder (unsigned patchNum)
{
  unsigned ch, ch2, ch3;

  if (strcmp(&patch_folder[0], &s_data[patchNum].folder[0]) != 0)
  {
    // Not in the right folder...

    while (patch_steps)
    {
      patch_packet[patch_size++] = 0x04;
      patch_packet[patch_size++] = 0x00;
      patch_packet[patch_size++] = 0x0A;
      patch_packet[patch_size++] = 0x00;
      patch_steps--;
    }

    if (s_data[patchNum].patch_folders[0] != 0x2E)
    {
      ch = 0;
      while (ch<s_data[patchNum].patch_folders_size)
      {
        memset (&patch_packet[patch_size], 0, 0x44);
        patch_packet[patch_size+0x00] = 0x44;
        patch_packet[patch_size+0x02] = 0x09;
        ch3 = 0;
        for (ch2=ch;ch2<s_data[patchNum].patch_folders_size;ch2++)
        {
          if (s_data[patchNum].patch_folders[ch2] == 0x00)
            break;
          ch3++;
        }
        strcat (&patch_packet[patch_size+0x04], &s_data[patchNum].patch_folders[ch]);
        ch += (ch3 + 1);
        patch_size += 0x44;
      }
    }
    memcpy (&patch_folder[0], &s_data[patchNum].folder, MAX_PATH);
    patch_steps = s_data[patchNum].patch_steps;
  }
}


int scandir(char *_path,BOOL recursive)
{
  HANDLE fh;
  char   path[MAX_PATH];
  char   tmppath[MAX_PATH];
  WIN32_FIND_DATA *fd;
  void *pd;
  FILE* pf;
  unsigned f_size, f_checksum, ch, ch2, ch3;

  now_folder ++;

  fd = malloc(sizeof(WIN32_FIND_DATA));

  fixpath(_path,path);
  strcat(path,"*");

  printf("Scanning: %s\n",path);

  fh = FindFirstFile((LPCSTR) path,fd);

  if(fh != INVALID_HANDLE_VALUE)
  {
    do
    {
      if(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      {
        if((0 != strcmp(fd->cFileName,".")) && (0 != strcmp(fd->cFileName,"..")))
        {
          fixpath(_path,tmppath);
          strcat(tmppath,fd->cFileName);
          fixpath(tmppath,tmppath);
          if(recursive)
            scandir(tmppath,recursive);
        }
      }
      else
      {
        fixpath(_path,tmppath);
        s_data[serverNumPatches].full_file_name[0] = 0;
        strcat (&s_data[serverNumPatches].full_file_name[0],&tmppath[0]);
        strcat (&s_data[serverNumPatches].full_file_name[0],fd->cFileName);
        f_size = ((fd->nFileSizeHigh * MAXDWORD)+fd->nFileSizeLow);
        pd = malloc (f_size);
        pf = fopen (&s_data[serverNumPatches].full_file_name[0], "rb");
        fread ( pd, 1, f_size, pf );
        fclose ( pf );
        f_checksum = CalculateChecksum ( pd, f_size );
        free ( pd );
        printf ("%s  Bytes: %u  Checksum: %08x\n", fd->cFileName, f_size, f_checksum );
        s_data[serverNumPatches].file_size  = f_size;
        s_data[serverNumPatches].checksum = f_checksum;
        s_data[serverNumPatches].file_name[0] = 0;
        strcat (&s_data[serverNumPatches].file_name[0],fd->cFileName);
        s_data[serverNumPatches].folder[0] = 0;
        strcat (&s_data[serverNumPatches].folder[0],&tmppath[0]);
        ch2 = 0;
        ch3 = 0;
        if (now_folder)
        {
          for (ch=0;ch<strlen(&tmppath[0]);ch++)
          {
            if (tmppath[ch] != 92)
              s_data[serverNumPatches].patch_folders[ch2++] = tmppath[ch];
            else
            {
              s_data[serverNumPatches].patch_folders[ch2++] = 0;
              _strlwr (&s_data[serverNumPatches].patch_folders[ch3]);
              if (strcmp(&s_data[serverNumPatches].patch_folders[ch3],"patches") == 0)
              {
                ch2 = ch3;
                s_data[serverNumPatches].patch_folders[ch2] = 0;
              }
              else
                ch3 = ch2;
            }
          }
          s_data[serverNumPatches].patch_folders_size = ch2;
        }
        else
        {
          s_data[serverNumPatches].patch_folders[0] = 0x2E;
          s_data[serverNumPatches].patch_folders[1] = 0;
          s_data[serverNumPatches].patch_folders_size = 1;
        }

        /*

        printf ("file patch\n\nfile_size: %u\n", s_data[serverNumPatches].file_size);
        printf ("checksum: %08x\n", s_data[serverNumPatches].checksum);
        printf ("full file name: %s\n", s_data[serverNumPatches].full_file_name);
        printf ("file name: %s\n", s_data[serverNumPatches].file_name);
        printf ("folder: %s\n", s_data[serverNumPatches].folder);
        printf ("patch_folders_size: %u\n", s_data[serverNumPatches].patch_folders_size);
        printf ("patch_steps: %u\n", s_data[serverNumPatches].patch_steps);

        */

        s_data[serverNumPatches++].patch_steps = now_folder;
        change_patch_folder (serverNumPatches - 1);
        ch = serverNumPatches - 1;
        memset (&patch_packet[patch_size], 0, 0x28);
        memcpy (&patch_packet[patch_size+4], &ch, 4);
        strcat (&patch_packet[patch_size+8], fd->cFileName);
        patch_packet[patch_size]   = 0x28;
        patch_packet[patch_size+2] = 0x0C;
        patch_size += 0x28;
      }
    }
    while(FindNextFile(fh,fd));
  }

  FindClose(fh);

  free (fd);

  now_folder--;

  return 1;
}

int fixpath(char *inpath, char *outpath)
{
  int   n=0;

  strcpy(outpath,inpath);

  while(inpath[n]) n++;

  if(inpath[n-1] != '\\')
  {
    strcat(outpath,"\\");
    return 1;
  }

  return 0;
}

LRESULT CALLBACK WndProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
  if(message == MYWM_NOTIFYICON)
  {
    switch (lParam)
    {
    case WM_LBUTTONDBLCLK:
      switch (wParam)
      {
      case 100:
        if (program_hidden)
        {
          program_hidden = 0;
          ShowWindow (consoleHwnd, SW_NORMAL);
          SetForegroundWindow (consoleHwnd);
          SetFocus(consoleHwnd);
        }
        else
        {
          program_hidden = 1;
          ShowWindow (consoleHwnd, SW_HIDE);
        }
        return TRUE;
        break;
      }
      break;
    }
  }
  return DefWindowProc( hwnd, message, wParam, lParam );
}

/********************************************************
**
**    main  :-
**
********************************************************/

int main( int argc, char * argv[] )
{
  unsigned ch,ch2;
  struct in_addr patch_in;
  struct in_addr data_in;
  struct sockaddr_in listen_in;
  unsigned listen_length;
  int patch_sockfd = -1, data_sockfd;
  int pkt_len, pkt_c, bytes_sent;
  unsigned short this_packet;
  unsigned short *w;
  unsigned short *w2;
  unsigned num_sends = 0;
  patch_data *pd;
  HINSTANCE hinst;
    NOTIFYICONDATA nid = {0};
  WNDCLASS wc = {0};
  HWND hwndWindow;
  MSG msg;

  FILE* fp;
  //int wserror;
  unsigned char tmprcv[TCP_BUFFER_SIZE];
  unsigned connectNum;
  unsigned to_send, checksum;
  int data_send, data_remaining;
  WSADATA winsock_data;

  consoleHwnd = GetConsoleWindow();
  hinst = GetModuleHandle(NULL);

  dp[0] = 0;

  strcat (&dp[0], "Tethealla Patch Server version ");
  strcat (&dp[0], SERVER_VERSION );
  strcat (&dp[0], " coded by Sodaboy");
  SetConsoleTitle (&dp[0]);

  printf ("\nTethealla Patch Server version %s  Copyright (C) 2008  Terry Chatman Jr.\n", SERVER_VERSION);
  printf ("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n");
  printf ("This program comes with ABSOLUTELY NO WARRANTY; for details\n");
  printf ("see section 15 in gpl-3.0.txt\n");
    printf ("This is free software, and you are welcome to redistribute it\n");
    printf ("under certain conditions; see gpl-3.0.txt for details.\n");

  for (ch=0;ch<5;ch++)
  {
    printf (".");
    Sleep (1000);
  }
  printf ("\n\n");

  data_remaining = 0;

  WSAStartup(MAKEWORD(1,1), &winsock_data);

  srand ( (unsigned) time(NULL) );

  printf ("Loading configuration from tethealla.ini ...");
  load_config_file();
  if (maxbytes)
    data_remaining = maxbytes;
  printf ("  OK!\n");
  printf ("\nPatch server parameters\n");
  printf ("///////////////////////\n");
  if (override_on)
    printf ("NOTE: IP override feature is turned on.\nThe server will bind to %u.%u.%u.%u but will send out the IP listed below.\n", overrideIP[0], overrideIP[1], overrideIP[2], overrideIP[3] );
  printf ("IP: %u.%u.%u.%u\n", serverIP[0], serverIP[1], serverIP[2], serverIP[3] );
  printf ("Patch Port: %u\n", serverPort - 1000 );
  printf ("Data Port: %u\n", serverPort - 999 );
  printf ("Maximum Connections: %u\n", serverMaxConnections );
  printf ("Upload speed: ");
  if (maxbytes)
    printf ("%u KB/s\n", maxbytes / 1024L);
  else
    printf ("Max\n");
  printf ("Allocating %u bytes of memory for connections...", sizeof (BANANA) * serverMaxConnections );
  for (ch=0;ch<serverMaxConnections;ch++)
  {
    connections[ch] = malloc ( sizeof (BANANA) );
    if ( !connections[ch] )
    {
      printf ("Out of memory!\n");
      exit (1);
    }
    initialize_connection (connections[ch]);
  }
  printf (" OK!\n\n");

  printf ("Preparing patch data...\n");
  printf ("Reading from folder \"patches\"...\n");

  memset (&patch_packet[patch_size], 0, 0x44);
  patch_packet[patch_size+0x00] = 0x44;
  patch_packet[patch_size+0x02] = 0x09;
  patch_packet[patch_size+0x04] = 0x2E;
  patch_size += 0x44;
  scandir ("patches",1);
  patch_packet[patch_size++] = 0x04;
  patch_packet[patch_size++] = 0x00;
  patch_packet[patch_size++] = 0x0A;
  patch_packet[patch_size++] = 0x00;਍  瀀愀琀挀栀开瀀愀挀欀攀琀嬀瀀愀琀挀栀开猀椀稀攀⬀⬀崀 㴀 　砀　㐀㬀ഀ਀  瀀愀琀挀栀开瀀愀挀欀攀琀嬀瀀愀琀挀栀开猀椀稀攀⬀⬀崀 㴀 　砀　　㬀ഀ਀  瀀愀琀挀栀开瀀愀挀欀攀琀嬀瀀愀琀挀栀开猀椀稀攀⬀⬀崀 㴀 　砀　䐀㬀ഀ਀  瀀愀琀挀栀开瀀愀挀欀攀琀嬀瀀愀琀挀栀开猀椀稀攀⬀⬀崀 㴀 　砀　　㬀ഀ਀  瀀爀椀渀琀昀 ⠀∀尀渀䐀漀渀攀℀尀渀尀渀∀⤀㬀ഀ਀ഀ਀  椀昀 ⠀℀猀攀爀瘀攀爀一甀洀倀愀琀挀栀攀猀⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀吀栀攀爀攀 愀爀攀 渀漀 瀀愀琀挀栀攀猀 琀漀 猀攀渀搀⸀尀渀夀漀甀 渀攀攀搀 愀琀 氀攀愀猀琀 漀渀攀 瀀愀琀挀栀 昀椀氀攀 琀漀 猀攀渀搀 漀爀 挀栀攀挀欀⸀尀渀∀⤀㬀ഀ਀    瀀爀椀渀琀昀 ⠀∀䠀椀琀 嬀䔀一吀䔀刀崀∀⤀㬀ഀ਀    攀砀椀琀 ⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀  瀀爀椀渀琀昀 ⠀∀䰀漀愀搀椀渀最 眀攀氀挀漀洀攀⸀琀砀琀 ⸀⸀⸀∀⤀㬀ഀ਀  昀瀀 㴀 昀漀瀀攀渀 ⠀∀眀攀氀挀漀洀攀⸀琀砀琀∀Ⰰ∀爀戀∀⤀㬀ഀ਀  椀昀 ⠀℀昀瀀⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀尀渀眀攀氀挀漀洀攀⸀琀砀琀 猀攀攀洀猀 琀漀 戀攀 洀椀猀猀椀渀最⸀尀渀倀氀攀愀猀攀 戀攀 猀甀爀攀 椀琀✀猀 椀渀 琀栀攀 猀愀洀攀 昀漀氀搀攀爀 愀猀 瀀愀琀挀栀开猀攀爀瘀攀爀⸀攀砀攀尀渀∀⤀㬀ഀ਀    瀀爀椀渀琀昀 ⠀∀䠀椀琀 嬀䔀一吀䔀刀崀∀⤀㬀ഀ਀    最攀琀猀 ⠀☀圀攀氀挀漀洀攀开䴀攀猀猀愀最攀嬀　崀⤀㬀ഀ਀    攀砀椀琀 ⠀㄀⤀㬀ഀ਀  紀ഀ਀  昀猀攀攀欀 ⠀ 昀瀀Ⰰ 　Ⰰ 匀䔀䔀䬀开䔀一䐀 ⤀㬀ഀ਀  挀栀 㴀 昀琀攀氀氀 ⠀ 昀瀀 ⤀㬀ഀ਀  昀猀攀攀欀 ⠀ 昀瀀Ⰰ 　Ⰰ 匀䔀䔀䬀开匀䔀吀 ⤀㬀ഀ਀  椀昀 ⠀ 挀栀 㸀 㐀　㤀㘀 ⤀ഀ਀     挀栀 㴀 㐀　㤀㘀㬀ഀ਀  昀爀攀愀搀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　崀Ⰰ ㄀Ⰰ 挀栀Ⰰ 昀瀀 ⤀㬀ഀ਀  昀挀氀漀猀攀 ⠀ 昀瀀 ⤀㬀ഀ਀ഀ਀  眀  㴀 ⠀甀渀猀椀最渀攀搀 猀栀漀爀琀⨀⤀ ☀倀愀挀欀攀琀䐀愀琀愀嬀　崀㬀ഀ਀  眀㈀ 㴀 ⠀甀渀猀椀最渀攀搀 猀栀漀爀琀⨀⤀ ☀圀攀氀挀漀洀攀开䴀攀猀猀愀最攀嬀　崀㬀ഀ਀  圀攀氀挀漀洀攀开䴀攀猀猀愀最攀开匀椀稀攀 㴀 　㬀ഀ਀  昀漀爀 ⠀ 挀栀㈀ 㴀 　㬀 挀栀㈀ 㰀 挀栀㬀 挀栀㈀ ⬀㴀 ㈀ ⤀ഀ਀  笀ഀ਀    椀昀 ⠀⨀眀 㴀㴀 　砀　　㈀㐀⤀ഀ਀      ⨀眀 㴀  　砀　　　㤀㬀 ⼀⼀ 䌀栀愀渀最攀 ␀ 琀漀 　砀　㤀ഀ਀    椀昀 ⠀⨀眀 ℀㴀 　砀　　　䐀⤀ഀ਀    笀ഀ਀      ⨀⠀眀㈀⬀⬀⤀ 㴀 ⨀眀㬀ഀ਀      圀攀氀挀漀洀攀开䴀攀猀猀愀最攀开匀椀稀攀 ⬀㴀 ㈀㬀ഀ਀    紀ഀ਀    眀⬀⬀㬀ഀ਀  紀ഀ਀ഀ਀  ⨀眀㈀ 㴀 　砀　　　　㬀ഀ਀ഀ਀  瀀爀椀渀琀昀 ⠀∀  ⠀─甀 戀礀琀攀猀⤀ 伀䬀℀尀渀尀渀∀Ⰰ 圀攀氀挀漀洀攀开䴀攀猀猀愀最攀开匀椀稀攀⤀㬀ഀ਀ഀ਀  ⼀⨀ 伀瀀攀渀 琀栀攀 倀匀伀 䈀䈀 倀愀琀挀栀 匀攀爀瘀攀爀 倀漀爀琀⸀⸀⸀ ⨀⼀ഀ਀ഀ਀  瀀爀椀渀琀昀 ⠀∀伀瀀攀渀椀渀最 猀攀爀瘀攀爀 瀀愀琀挀栀 瀀漀爀琀 ─甀 昀漀爀 挀漀渀渀攀挀琀椀漀渀猀⸀尀渀∀Ⰰ 猀攀爀瘀攀爀倀漀爀琀 ⴀ ㄀　　　⤀㬀ഀ਀ഀ਀⌀椀昀搀攀昀 唀匀䔀䄀䐀䐀刀开䄀一夀ഀ਀  瀀愀琀挀栀开椀渀⸀猀开愀搀搀爀 㴀 䤀一䄀䐀䐀刀开䄀一夀㬀ഀ਀⌀攀氀猀攀ഀ਀  椀昀 ⠀漀瘀攀爀爀椀搀攀开漀渀⤀ഀ਀    洀攀洀挀瀀礀 ⠀☀瀀愀琀挀栀开椀渀⸀猀开愀搀搀爀Ⰰ ☀漀瘀攀爀爀椀搀攀䤀倀嬀　崀Ⰰ 㐀 ⤀㬀ഀ਀  攀氀猀攀ഀ਀    洀攀洀挀瀀礀 ⠀☀瀀愀琀挀栀开椀渀⸀猀开愀搀搀爀Ⰰ ☀猀攀爀瘀攀爀䤀倀嬀　崀Ⰰ 㐀 ⤀㬀ഀ਀⌀攀渀搀椀昀ഀ਀ഀ਀  瀀愀琀挀栀开猀漀挀欀昀搀 㴀 琀挀瀀开猀漀挀欀开漀瀀攀渀⠀ 瀀愀琀挀栀开椀渀Ⰰ 猀攀爀瘀攀爀倀漀爀琀 ⴀ ㄀　　　 ⤀㬀ഀ਀ഀ਀  琀挀瀀开氀椀猀琀攀渀 ⠀瀀愀琀挀栀开猀漀挀欀昀搀⤀㬀ഀ਀ഀ਀⌀椀昀搀攀昀 唀匀䔀䄀䐀䐀刀开䄀一夀ഀ਀  搀愀琀愀开椀渀⸀猀开愀搀搀爀 㴀 䤀一䄀䐀䐀刀开䄀一夀㬀ഀ਀⌀攀氀猀攀ഀ਀  椀昀 ⠀漀瘀攀爀爀椀搀攀开漀渀⤀ഀ਀    洀攀洀挀瀀礀 ⠀☀搀愀琀愀开椀渀⸀猀开愀搀搀爀Ⰰ ☀漀瘀攀爀爀椀搀攀䤀倀嬀　崀Ⰰ 㐀 ⤀㬀ഀ਀  攀氀猀攀ഀ਀    洀攀洀挀瀀礀 ⠀☀搀愀琀愀开椀渀⸀猀开愀搀搀爀Ⰰ ☀猀攀爀瘀攀爀䤀倀嬀　崀Ⰰ 㐀 ⤀㬀ഀ਀⌀攀渀搀椀昀ഀ਀ഀ਀  瀀爀椀渀琀昀 ⠀∀伀瀀攀渀椀渀最 猀攀爀瘀攀爀 搀愀琀愀 瀀漀爀琀 ─甀 昀漀爀 挀漀渀渀攀挀琀椀漀渀猀⸀尀渀∀Ⰰ 猀攀爀瘀攀爀倀漀爀琀 ⴀ 㤀㤀㤀⤀㬀ഀ਀ഀ਀  搀愀琀愀开猀漀挀欀昀搀 㴀 琀挀瀀开猀漀挀欀开漀瀀攀渀⠀ 搀愀琀愀开椀渀Ⰰ 猀攀爀瘀攀爀倀漀爀琀 ⴀ 㤀㤀㤀 ⤀㬀ഀ਀ഀ਀  琀挀瀀开氀椀猀琀攀渀 ⠀搀愀琀愀开猀漀挀欀昀搀⤀㬀ഀ਀ഀ਀  椀昀 ⠀⠀瀀愀琀挀栀开猀漀挀欀昀搀㰀　⤀ 簀簀 ⠀搀愀琀愀开猀漀挀欀昀搀㰀　⤀⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀䘀愀椀氀攀搀 琀漀 漀瀀攀渀 瀀漀爀琀 昀漀爀 挀漀渀渀攀挀琀椀漀渀猀⸀尀渀∀⤀㬀ഀ਀    攀砀椀琀 ⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀  瀀爀椀渀琀昀 ⠀∀尀渀䰀椀猀琀攀渀椀渀最⸀⸀⸀尀渀∀⤀㬀ഀ਀ഀ਀  眀挀⸀栀戀爀䈀愀挀欀最爀漀甀渀搀 㴀⠀䠀䈀刀唀匀䠀⤀䜀攀琀匀琀漀挀欀伀戀樀攀挀琀⠀圀䠀䤀吀䔀开䈀刀唀匀䠀⤀㬀ഀ਀  眀挀⸀栀䤀挀漀渀 㴀 䰀漀愀搀䤀挀漀渀⠀ 栀椀渀猀琀Ⰰ 䤀䐀䤀开䄀倀倀䰀䤀䌀䄀吀䤀伀一 ⤀㬀ഀ਀  眀挀⸀栀䌀甀爀猀漀爀 㴀 䰀漀愀搀䌀甀爀猀漀爀⠀ 栀椀渀猀琀Ⰰ 䤀䐀䌀开䄀刀刀伀圀 ⤀㬀ഀ਀  眀挀⸀栀䤀渀猀琀愀渀挀攀 㴀 栀椀渀猀琀㬀ഀ਀  眀挀⸀氀瀀昀渀圀渀搀倀爀漀挀 㴀 圀渀搀倀爀漀挀㬀ഀ਀  眀挀⸀氀瀀猀稀䌀氀愀猀猀一愀洀攀 㴀 ∀猀漀搀愀戀漀礀∀㬀ഀ਀  眀挀⸀猀琀礀氀攀 㴀 䌀匀开䠀刀䔀䐀刀䄀圀 簀 䌀匀开嘀刀䔀䐀刀䄀圀㬀ഀ਀ഀ਀  椀昀 ⠀℀ 刀攀最椀猀琀攀爀䌀氀愀猀猀⠀ ☀眀挀 ⤀ ⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀刀攀最椀猀琀攀爀䌀氀愀猀猀 昀愀椀氀甀爀攀⸀尀渀∀⤀㬀ഀ਀    攀砀椀琀 ⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀  栀眀渀搀圀椀渀搀漀眀 㴀 䌀爀攀愀琀攀圀椀渀搀漀眀 ⠀∀猀漀搀愀戀漀礀∀Ⰰ∀栀椀搀搀攀渀 眀椀渀搀漀眀∀Ⰰ 圀匀开䴀䤀一䤀䴀䤀娀䔀Ⰰ ㄀Ⰰ ㄀Ⰰ ㄀Ⰰ ㄀Ⰰഀ਀    一唀䰀䰀Ⰰഀ਀    一唀䰀䰀Ⰰഀ਀    栀椀渀猀琀Ⰰഀ਀    一唀䰀䰀 ⤀㬀ഀ਀ഀ਀  椀昀 ⠀℀栀眀渀搀圀椀渀搀漀眀⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀䘀愀椀氀攀搀 琀漀 挀爀攀愀琀攀 眀椀渀搀漀眀⸀∀⤀㬀ഀ਀    攀砀椀琀 ⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀  匀栀漀眀圀椀渀搀漀眀 ⠀ 栀眀渀搀圀椀渀搀漀眀Ⰰ 匀圀开䠀䤀䐀䔀 ⤀㬀ഀ਀  唀瀀搀愀琀攀圀椀渀搀漀眀 ⠀ 栀眀渀搀圀椀渀搀漀眀 ⤀㬀ഀ਀  匀栀漀眀圀椀渀搀漀眀 ⠀ 挀漀渀猀漀氀攀䠀眀渀搀Ⰰ 匀圀开䠀䤀䐀䔀 ⤀㬀ഀ਀  唀瀀搀愀琀攀圀椀渀搀漀眀 ⠀ 挀漀渀猀漀氀攀䠀眀渀搀 ⤀㬀ഀ਀ഀ਀    渀椀搀⸀挀戀匀椀稀攀        㴀 猀椀稀攀漀昀⠀渀椀搀⤀㬀ഀ਀  渀椀搀⸀栀圀渀搀        㴀 栀眀渀搀圀椀渀搀漀眀㬀ഀ਀  渀椀搀⸀甀䤀䐀         㴀 ㄀　　㬀ഀ਀  渀椀搀⸀甀䌀愀氀氀戀愀挀欀䴀攀猀猀愀最攀  㴀 䴀夀圀䴀开一伀吀䤀䘀夀䤀䌀伀一㬀ഀ਀  渀椀搀⸀甀䘀氀愀最猀        㴀 一䤀䘀开䴀䔀匀匀䄀䜀䔀簀一䤀䘀开䤀䌀伀一簀一䤀䘀开吀䤀倀㬀ഀ਀    渀椀搀⸀栀䤀挀漀渀       㴀 䰀漀愀搀䤀挀漀渀⠀栀椀渀猀琀Ⰰ 䴀䄀䬀䔀䤀一吀刀䔀匀伀唀刀䌀䔀⠀䤀䐀䤀开䤀䌀伀一㄀⤀⤀㬀ഀ਀  渀椀搀⸀猀稀吀椀瀀嬀　崀 㴀 　㬀ഀ਀  猀琀爀挀愀琀 ⠀☀渀椀搀⸀猀稀吀椀瀀嬀　崀Ⰰ ∀吀攀琀栀攀愀氀氀愀 倀愀琀挀栀 ∀⤀㬀ഀ਀  猀琀爀挀愀琀 ⠀☀渀椀搀⸀猀稀吀椀瀀嬀　崀Ⰰ 匀䔀刀嘀䔀刀开嘀䔀刀匀䤀伀一⤀㬀ഀ਀  猀琀爀挀愀琀 ⠀☀渀椀搀⸀猀稀吀椀瀀嬀　崀Ⰰ ∀ ⴀ 䐀漀甀戀氀攀 挀氀椀挀欀 琀漀 猀栀漀眀⼀栀椀搀攀∀⤀㬀ഀ਀    匀栀攀氀氀开一漀琀椀昀礀䤀挀漀渀⠀一䤀䴀开䄀䐀䐀Ⰰ ☀渀椀搀⤀㬀ഀ਀ഀ਀ഀ਀  昀漀爀 ⠀㬀㬀⤀ഀ਀  笀ഀ਀    椀渀琀 渀昀搀猀 㴀 　㬀ഀ਀ഀ਀    ⼀⨀ 倀爀漀挀攀猀猀 琀栀攀 猀礀猀琀攀洀 琀爀愀礀 椀挀漀渀 ⨀⼀ഀ਀ഀ਀    椀昀 ⠀ 倀攀攀欀䴀攀猀猀愀最攀⠀ ☀洀猀最Ⰰ 栀眀渀搀圀椀渀搀漀眀Ⰰ 　Ⰰ 　Ⰰ ㄀ ⤀ ⤀ഀ਀    笀ഀ਀      吀爀愀渀猀氀愀琀攀䴀攀猀猀愀最攀⠀☀洀猀最⤀㬀ഀ਀      䐀椀猀瀀愀琀挀栀䴀攀猀猀愀最攀⠀☀洀猀最⤀㬀ഀ਀    紀ഀ਀ഀ਀    ⼀⨀ 倀椀渀最 瀀漀渀最㼀℀ ⨀⼀ഀ਀ഀ਀    猀攀爀瘀攀爀琀椀洀攀 㴀 琀椀洀攀⠀一唀䰀䰀⤀㬀ഀ਀ഀ਀    椀昀 ⠀⠀洀愀砀戀礀琀攀猀⤀ ☀☀ ⠀猀攀渀搀琀椀洀攀 ℀㴀 ⠀甀渀猀椀最渀攀搀⤀ 猀攀爀瘀攀爀琀椀洀攀⤀⤀ഀ਀    笀ഀ਀      猀攀渀搀琀椀洀攀  㴀 ⠀甀渀猀椀最渀攀搀⤀ 猀攀爀瘀攀爀琀椀洀攀㬀ഀ਀      搀愀琀愀开爀攀洀愀椀渀椀渀最 㴀 洀愀砀戀礀琀攀猀㬀ഀ਀    紀ഀ਀ഀ਀    ⼀⨀ 䌀氀攀愀爀 猀漀挀欀攀琀 愀挀琀椀瘀椀琀礀 昀氀愀最猀⸀ ⨀⼀ഀ਀ഀ਀    䘀䐀开娀䔀刀伀 ⠀☀刀攀愀搀䘀䐀猀⤀㬀ഀ਀    䘀䐀开娀䔀刀伀 ⠀☀圀爀椀琀攀䘀䐀猀⤀㬀ഀ਀    䘀䐀开娀䔀刀伀 ⠀☀䔀砀挀攀瀀琀䘀䐀猀⤀㬀ഀ਀ഀ਀    渀甀洀开猀攀渀搀猀 㴀 　㬀ഀ਀ഀ਀    昀漀爀 ⠀挀栀㴀　㬀挀栀㰀猀攀爀瘀攀爀一甀洀䌀漀渀渀攀挀琀椀漀渀猀㬀挀栀⬀⬀⤀ഀ਀    笀ഀ਀      挀漀渀渀攀挀琀一甀洀 㴀 猀攀爀瘀攀爀䌀漀渀渀攀挀琀椀漀渀䰀椀猀琀嬀挀栀崀㬀ഀ਀      眀漀爀欀䌀漀渀渀攀挀琀 㴀 挀漀渀渀攀挀琀椀漀渀猀嬀挀漀渀渀攀挀琀一甀洀崀㬀ഀ਀ഀ਀      椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀 㸀㴀 　⤀ഀ਀      笀ഀ਀        椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀⤀ഀ਀        笀ഀ਀          洀攀洀挀瀀礀 ⠀☀琀栀椀猀开瀀愀挀欀攀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀爀攀愀搀崀Ⰰ ㈀⤀㬀ഀ਀          洀攀洀挀瀀礀 ⠀☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀搀攀挀爀礀瀀琀戀甀昀嬀　崀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀爀攀愀搀崀Ⰰ 琀栀椀猀开瀀愀挀欀攀琀⤀㬀ഀ਀ഀ਀          ⼀⼀搀攀戀甀最 ⠀∀刀攀挀攀椀瘀攀搀 昀爀漀洀 挀氀椀攀渀琀㨀─甀∀Ⰰ 琀栀椀猀开瀀愀挀欀攀琀⤀㬀ഀ਀          ⼀⼀搀椀猀瀀氀愀礀开瀀愀挀欀攀琀 ⠀☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀搀攀挀爀礀瀀琀戀甀昀嬀　崀Ⰰ 琀栀椀猀开瀀愀挀欀攀琀⤀㬀ഀ਀ഀ਀          椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀琀挀栀⤀ഀ਀            䐀愀琀愀倀爀漀挀攀猀猀倀愀挀欀攀琀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀          攀氀猀攀ഀ਀            倀愀琀挀栀倀爀漀挀攀猀猀倀愀挀欀攀琀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀ഀ਀          眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀爀攀愀搀 ⬀㴀 琀栀椀猀开瀀愀挀欀攀琀㬀ഀ਀          椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀爀攀愀搀 㴀㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀⤀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀爀攀愀搀 㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀 㴀 　㬀ഀ਀        紀ഀ਀ഀ਀        椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀氀愀猀琀吀椀挀欀 ℀㴀 ⠀甀渀猀椀最渀攀搀⤀ 猀攀爀瘀攀爀琀椀洀攀⤀ഀ਀        笀ഀ਀          椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀氀愀猀琀吀椀挀欀 㸀 ⠀甀渀猀椀最渀攀搀⤀ 猀攀爀瘀攀爀琀椀洀攀⤀ഀ਀            挀栀㈀ 㴀 ㄀㬀ഀ਀          攀氀猀攀ഀ਀            挀栀㈀ 㴀 ㄀ ⬀ ⠀⠀甀渀猀椀最渀攀搀⤀ 猀攀爀瘀攀爀琀椀洀攀 ⴀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀氀愀猀琀吀椀挀欀⤀㬀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀氀愀猀琀吀椀挀欀 㴀 ⠀甀渀猀椀最渀攀搀⤀ 猀攀爀瘀攀爀琀椀洀攀㬀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀猀匀攀挀 ⼀㴀 挀栀㈀㬀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀琀漀䈀礀琀攀猀匀攀挀 ⼀㴀 挀栀㈀㬀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀昀爀漀洀䈀礀琀攀猀匀攀挀 ⼀㴀 挀栀㈀㬀ഀ਀        紀ഀ਀ഀ਀        䘀䐀开匀䔀吀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀刀攀愀搀䘀䐀猀⤀㬀ഀ਀        渀昀搀猀 㴀 洀愀砀 ⠀渀昀搀猀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀⤀㬀ഀ਀        䘀䐀开匀䔀吀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀䔀砀挀攀瀀琀䘀䐀猀⤀㬀ഀ਀        渀昀搀猀 㴀 洀愀砀 ⠀渀昀搀猀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀⤀㬀ഀ਀ഀ਀        椀昀 ⠀⠀℀洀愀砀戀礀琀攀猀⤀ 簀簀 ⠀搀愀琀愀开爀攀洀愀椀渀椀渀最⤀⤀ഀ਀        笀ഀ਀          椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀搀愀琀愀 ⴀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀眀爀椀琀琀攀渀⤀ഀ਀          笀ഀ਀            䘀䐀开匀䔀吀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀圀爀椀琀攀䘀䐀猀⤀㬀ഀ਀            渀昀搀猀 㴀 洀愀砀 ⠀渀昀搀猀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀⤀㬀ഀ਀          紀ഀ਀          攀氀猀攀ഀ਀          笀ഀ਀            ⼀⼀ 匀攀渀搀 爀攀洀愀椀渀椀渀最 瀀愀琀挀栀 搀愀琀愀 栀攀爀攀⸀⸀⸀ഀ਀            ⼀⼀ 椀昀 猀攀渀搀椀渀最开昀椀氀攀猀 愀渀搀 猀琀甀昀昀 氀攀昀琀 琀漀 最漀ഀ਀            椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀渀搀椀渀最开昀椀氀攀猀⤀ഀ਀            笀ഀ਀              渀甀洀开猀攀渀搀猀⬀⬀㬀ഀ਀              瀀搀 㴀 ☀猀开搀愀琀愀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀开搀愀琀愀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀甀爀爀攀渀琀开昀椀氀攀崀崀㬀ഀ਀              昀瀀 㴀 昀漀瀀攀渀 ⠀☀瀀搀ⴀ㸀昀甀氀氀开昀椀氀攀开渀愀洀攀嬀　崀Ⰰ ∀爀戀∀⤀㬀ഀ਀              昀猀攀攀欀 ⠀昀瀀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀昀椀氀攀开椀渀搀攀砀Ⰰ 匀䔀䔀䬀开匀䔀吀⤀㬀ഀ਀              琀漀开猀攀渀搀 㴀 瀀搀ⴀ㸀昀椀氀攀开猀椀稀攀 ⴀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀昀椀氀攀开椀渀搀攀砀㬀ഀ਀              椀昀 ⠀琀漀开猀攀渀搀 㸀 ㈀㐀㔀㜀㘀⤀ഀ਀                琀漀开猀攀渀搀 㴀 ㈀㐀㔀㜀㘀㬀ഀ਀              昀爀攀愀搀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　砀㄀　崀Ⰰ ㄀Ⰰ 琀漀开猀攀渀搀Ⰰ 昀瀀⤀㬀ഀ਀              昀挀氀漀猀攀 ⠀昀瀀⤀㬀ഀ਀              眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀昀椀氀攀开椀渀搀攀砀 ⬀㴀 琀漀开猀攀渀搀㬀ഀ਀              挀栀攀挀欀猀甀洀 㴀 䌀愀氀挀甀氀愀琀攀䌀栀攀挀欀猀甀洀 ⠀ ☀倀愀挀欀攀琀䐀愀琀愀嬀　砀㄀　崀Ⰰ 琀漀开猀攀渀搀⤀㬀ഀ਀              洀攀洀猀攀琀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　砀　　崀Ⰰ 　Ⰰ 　砀㄀　⤀㬀ഀ਀              倀愀挀欀攀琀䐀愀琀愀嬀　砀　㈀崀 㴀 　砀　㜀㬀ഀ਀              洀攀洀挀瀀礀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　砀　㐀崀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀栀甀渀欀Ⰰ 㐀⤀㬀ഀ਀              洀攀洀挀瀀礀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　砀　㠀崀Ⰰ ☀挀栀攀挀欀猀甀洀Ⰰ 㐀⤀㬀ഀ਀              洀攀洀挀瀀礀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　砀　䌀崀Ⰰ ☀琀漀开猀攀渀搀Ⰰ  㐀⤀㬀ഀ਀              琀漀开猀攀渀搀 ⬀㴀 　砀㄀　㬀ഀ਀              眀栀椀氀攀 ⠀琀漀开猀攀渀搀 ─ 㐀⤀ഀ਀                倀愀挀欀攀琀䐀愀琀愀嬀琀漀开猀攀渀搀⬀⬀崀 㴀 　砀　　㬀ഀ਀              洀攀洀挀瀀礀 ⠀☀倀愀挀欀攀琀䐀愀琀愀嬀　砀　　崀Ⰰ ☀琀漀开猀攀渀搀Ⰰ ㈀⤀㬀ഀ਀              挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀爀瘀攀爀开挀椀瀀栀攀爀㬀ഀ਀              攀渀挀爀礀瀀琀挀漀瀀礀 ⠀眀漀爀欀䌀漀渀渀攀挀琀Ⰰ ☀倀愀挀欀攀琀䐀愀琀愀嬀　砀　　崀Ⰰ 琀漀开猀攀渀搀⤀㬀ഀ਀              眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀栀甀渀欀⬀⬀㬀ഀ਀              椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀昀椀氀攀开椀渀搀攀砀 㴀㴀 瀀搀ⴀ㸀昀椀氀攀开猀椀稀攀⤀ഀ਀              笀ഀ਀                ⼀⼀ 䘀椀氀攀✀猀 搀漀渀攀⸀⸀⸀ഀ਀                洀攀洀猀攀琀 ⠀☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀Ⰰ 　Ⰰ 㠀⤀㬀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀 㴀 　砀　㠀㬀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㈀崀 㴀 　砀　㠀㬀ഀ਀                挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀爀瘀攀爀开挀椀瀀栀攀爀㬀ഀ਀                攀渀挀爀礀瀀琀挀漀瀀礀 ⠀眀漀爀欀䌀漀渀渀攀挀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀Ⰰ 㠀⤀㬀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀栀甀渀欀 㴀 　㬀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀昀椀氀攀开椀渀搀攀砀 㴀 　㬀ഀ਀                ⼀⼀ 䄀爀攀 眀攀 挀漀洀瀀氀攀琀攀氀礀 搀漀渀攀㼀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀甀爀爀攀渀琀开昀椀氀攀⬀⬀㬀ഀ਀                椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀甀爀爀攀渀琀开昀椀氀攀 㴀㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀昀椀氀攀猀开琀漀开猀攀渀搀⤀ഀ਀                笀ഀ਀                  ⼀⼀ 䠀攀氀氀 礀攀愀栀 眀攀 愀爀攀℀ഀ਀                  眀栀椀氀攀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀琀挀栀开猀琀攀瀀猀⤀ഀ਀                  笀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀 㴀 　砀　㐀㬀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㄀崀 㴀 　砀　　㬀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㈀崀 㴀 　砀　䄀㬀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㌀崀 㴀 　砀　　㬀ഀ਀                    挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀爀瘀攀爀开挀椀瀀栀攀爀㬀ഀ਀                    攀渀挀爀礀瀀琀挀漀瀀礀 ⠀眀漀爀欀䌀漀渀渀攀挀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀Ⰰ 㐀⤀㬀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀琀挀栀开猀琀攀瀀猀ⴀⴀ㬀ഀ਀                  紀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀渀搀椀渀最开昀椀氀攀猀 㴀 　㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀 㴀 　砀　㐀㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㄀崀 㴀 　砀　　㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㈀崀 㴀 　砀　䄀㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㌀崀 㴀 　砀　　㬀ഀ਀                  挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀爀瘀攀爀开挀椀瀀栀攀爀㬀ഀ਀                  攀渀挀爀礀瀀琀挀漀瀀礀 ⠀眀漀爀欀䌀漀渀渀攀挀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀Ⰰ 㐀⤀㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀 㴀 　砀　㐀㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㄀崀 㴀 　砀　　㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㈀崀 㴀 　砀㄀㈀㬀ഀ਀                  眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　㌀崀 㴀 　砀　　㬀ഀ਀                  挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀爀瘀攀爀开挀椀瀀栀攀爀㬀ഀ਀                  攀渀挀爀礀瀀琀挀漀瀀礀 ⠀眀漀爀欀䌀漀渀渀攀挀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀渀挀爀礀瀀琀戀甀昀嬀　砀　　崀Ⰰ 㐀⤀㬀ഀ਀                紀ഀ਀                攀氀猀攀ഀ਀                  挀栀愀渀最攀开挀氀椀攀渀琀开昀漀氀搀攀爀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀开搀愀琀愀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀甀爀爀攀渀琀开昀椀氀攀崀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀              紀ഀ਀            紀ഀ਀          紀ഀ਀        紀ഀ਀      紀ഀ਀    紀ഀ਀ഀ਀    䘀䐀开匀䔀吀 ⠀瀀愀琀挀栀开猀漀挀欀昀搀Ⰰ ☀刀攀愀搀䘀䐀猀⤀㬀ഀ਀    渀昀搀猀 㴀 洀愀砀 ⠀渀昀搀猀Ⰰ 瀀愀琀挀栀开猀漀挀欀昀搀⤀㬀ഀ਀    䘀䐀开匀䔀吀 ⠀搀愀琀愀开猀漀挀欀昀搀Ⰰ ☀刀攀愀搀䘀䐀猀⤀㬀ഀ਀    渀昀搀猀 㴀 洀愀砀 ⠀渀昀搀猀Ⰰ 搀愀琀愀开猀漀挀欀昀搀⤀㬀ഀ਀ഀ਀    ⼀⨀ 䌀栀攀挀欀 猀漀挀欀攀琀猀 昀漀爀 愀挀琀椀瘀椀琀礀⸀ ⨀⼀ഀ਀ഀ਀    椀昀 ⠀ 猀攀氀攀挀琀 ⠀ 渀昀搀猀 ⬀ ㄀Ⰰ ☀刀攀愀搀䘀䐀猀Ⰰ ☀圀爀椀琀攀䘀䐀猀Ⰰ ☀䔀砀挀攀瀀琀䘀䐀猀Ⰰ ☀猀攀氀攀挀琀开琀椀洀攀漀甀琀 ⤀ 㸀 　 ⤀ഀ਀    笀ഀ਀      椀昀 ⠀䘀䐀开䤀匀匀䔀吀 ⠀瀀愀琀挀栀开猀漀挀欀昀搀Ⰰ ☀刀攀愀搀䘀䐀猀⤀⤀ഀ਀      笀ഀ਀        ⼀⼀ 匀漀洀攀漀渀攀✀猀 愀琀琀攀洀瀀琀椀渀最 琀漀 挀漀渀渀攀挀琀 琀漀 琀栀攀 瀀愀琀挀栀 猀攀爀瘀攀爀⸀ഀ਀        挀栀 㴀 昀爀攀攀开挀漀渀渀攀挀琀椀漀渀⠀⤀㬀ഀ਀        椀昀 ⠀挀栀 ℀㴀 　砀䘀䘀䘀䘀⤀ഀ਀        笀ഀ਀          氀椀猀琀攀渀开氀攀渀最琀栀 㴀 猀椀稀攀漀昀 ⠀氀椀猀琀攀渀开椀渀⤀㬀ഀ਀          眀漀爀欀䌀漀渀渀攀挀琀 㴀 挀漀渀渀攀挀琀椀漀渀猀嬀挀栀崀㬀ഀ਀          椀昀 ⠀ ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀 㴀 琀挀瀀开愀挀挀攀瀀琀 ⠀ 瀀愀琀挀栀开猀漀挀欀昀搀Ⰰ ⠀猀琀爀甀挀琀 猀漀挀欀愀搀搀爀⨀⤀ ☀氀椀猀琀攀渀开椀渀Ⰰ ☀氀椀猀琀攀渀开氀攀渀最琀栀 ⤀ ⤀ 㸀㴀 　 ⤀ഀ਀          笀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀漀渀渀攀挀琀椀漀渀开椀渀搀攀砀 㴀 挀栀㬀ഀ਀            猀攀爀瘀攀爀䌀漀渀渀攀挀琀椀漀渀䰀椀猀琀嬀猀攀爀瘀攀爀一甀洀䌀漀渀渀攀挀琀椀漀渀猀⬀⬀崀 㴀 挀栀㬀ഀ਀            洀攀洀挀瀀礀 ⠀ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀䤀倀开䄀搀搀爀攀猀猀嬀　崀Ⰰ 椀渀攀琀开渀琀漀愀 ⠀氀椀猀琀攀渀开椀渀⸀猀椀渀开愀搀搀爀⤀Ⰰ ㄀㘀 ⤀㬀ഀ਀            瀀爀椀渀琀昀 ⠀∀䄀挀挀攀瀀琀攀搀 倀䄀吀䌀䠀 挀漀渀渀攀挀琀椀漀渀 昀爀漀洀 ─猀㨀─甀尀渀∀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀䤀倀开䄀搀搀爀攀猀猀Ⰰ 氀椀猀琀攀渀开椀渀⸀猀椀渀开瀀漀爀琀 ⤀㬀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀琀挀栀 㴀 　㬀ഀ਀            猀琀愀爀琀开攀渀挀爀礀瀀琀椀漀渀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀          紀ഀ਀        紀ഀ਀      紀ഀ਀ഀ਀ഀ਀      椀昀 ⠀䘀䐀开䤀匀匀䔀吀 ⠀搀愀琀愀开猀漀挀欀昀搀Ⰰ ☀刀攀愀搀䘀䐀猀⤀⤀ഀ਀      笀ഀ਀        ⼀⼀ 匀漀洀攀漀渀攀✀猀 愀琀琀攀洀瀀琀椀渀最 琀漀 挀漀渀渀攀挀琀 琀漀 琀栀攀 瀀愀琀挀栀 猀攀爀瘀攀爀⸀ഀ਀        挀栀 㴀 昀爀攀攀开挀漀渀渀攀挀琀椀漀渀⠀⤀㬀ഀ਀        椀昀 ⠀挀栀 ℀㴀 　砀䘀䘀䘀䘀⤀ഀ਀        笀ഀ਀          氀椀猀琀攀渀开氀攀渀最琀栀 㴀 猀椀稀攀漀昀 ⠀氀椀猀琀攀渀开椀渀⤀㬀ഀ਀          眀漀爀欀䌀漀渀渀攀挀琀 㴀 挀漀渀渀攀挀琀椀漀渀猀嬀挀栀崀㬀ഀ਀          椀昀 ⠀ ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀 㴀 琀挀瀀开愀挀挀攀瀀琀 ⠀ 搀愀琀愀开猀漀挀欀昀搀Ⰰ ⠀猀琀爀甀挀琀 猀漀挀欀愀搀搀爀⨀⤀ ☀氀椀猀琀攀渀开椀渀Ⰰ ☀氀椀猀琀攀渀开氀攀渀最琀栀 ⤀ ⤀ 㸀㴀 　 ⤀ഀ਀          笀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀漀渀渀攀挀琀椀漀渀开椀渀搀攀砀 㴀 挀栀㬀ഀ਀            猀攀爀瘀攀爀䌀漀渀渀攀挀琀椀漀渀䰀椀猀琀嬀猀攀爀瘀攀爀一甀洀䌀漀渀渀攀挀琀椀漀渀猀⬀⬀崀 㴀 挀栀㬀ഀ਀            洀攀洀挀瀀礀 ⠀ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀䤀倀开䄀搀搀爀攀猀猀嬀　崀Ⰰ 椀渀攀琀开渀琀漀愀 ⠀氀椀猀琀攀渀开椀渀⸀猀椀渀开愀搀搀爀⤀Ⰰ ㄀㘀 ⤀㬀ഀ਀            瀀爀椀渀琀昀 ⠀∀䄀挀挀攀瀀琀攀搀 䐀䄀吀䄀 挀漀渀渀攀挀琀椀漀渀 昀爀漀洀 ─猀㨀─甀尀渀∀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀䤀倀开䄀搀搀爀攀猀猀Ⰰ 氀椀猀琀攀渀开椀渀⸀猀椀渀开瀀漀爀琀 ⤀㬀ഀ਀            眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀琀挀栀 㴀 ㄀㬀ഀ਀            猀琀愀爀琀开攀渀挀爀礀瀀琀椀漀渀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀          紀ഀ਀        紀ഀ਀      紀ഀ਀ഀ਀ഀ਀      ⼀⼀ 倀爀漀挀攀猀猀 挀氀椀攀渀琀 挀漀渀渀攀挀琀椀漀渀猀ഀ਀ഀ਀      昀漀爀 ⠀挀栀㴀　㬀挀栀㰀猀攀爀瘀攀爀一甀洀䌀漀渀渀攀挀琀椀漀渀猀㬀挀栀⬀⬀⤀ഀ਀      笀ഀ਀        挀漀渀渀攀挀琀一甀洀 㴀 猀攀爀瘀攀爀䌀漀渀渀攀挀琀椀漀渀䰀椀猀琀嬀挀栀崀㬀ഀ਀        眀漀爀欀䌀漀渀渀攀挀琀 㴀 挀漀渀渀攀挀琀椀漀渀猀嬀挀漀渀渀攀挀琀一甀洀崀㬀ഀ਀ഀ਀        椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀 㸀㴀 　⤀ഀ਀        笀ഀ਀          椀昀 ⠀䘀䐀开䤀匀匀䔀吀⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀䔀砀挀攀瀀琀䘀䐀猀⤀⤀ ⼀⼀ 䔀砀挀攀瀀琀椀漀渀㼀ഀ਀            椀渀椀琀椀愀氀椀稀攀开挀漀渀渀攀挀琀椀漀渀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀ഀ਀          椀昀 ⠀䘀䐀开䤀匀匀䔀吀⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀刀攀愀搀䘀䐀猀⤀⤀ഀ਀          笀ഀ਀            ⼀⼀ 刀攀愀搀 猀栀椀琀⸀ഀ਀            椀昀 ⠀ ⠀ 瀀欀琀开氀攀渀 㴀 爀攀挀瘀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀琀洀瀀爀挀瘀嬀　崀Ⰰ 吀䌀倀开䈀唀䘀䘀䔀刀开匀䤀娀䔀 ⴀ ㄀Ⰰ 　⤀ ⤀ 㰀㴀 　 ⤀ഀ਀            笀ഀ਀              ⼀⨀ഀ਀              眀猀攀爀爀漀爀 㴀 圀匀䄀䜀攀琀䰀愀猀琀䔀爀爀漀爀⠀⤀㬀ഀ਀              瀀爀椀渀琀昀 ⠀∀䌀漀甀氀搀 渀漀琀 爀攀愀搀 搀愀琀愀 昀爀漀洀 挀氀椀攀渀琀⸀⸀⸀尀渀∀⤀㬀ഀ਀              瀀爀椀渀琀昀 ⠀∀匀漀挀欀攀琀 䔀爀爀漀爀 ─甀⸀尀渀∀Ⰰ 眀猀攀爀爀漀爀 ⤀㬀ഀ਀              ⨀⼀ഀ਀              椀渀椀琀椀愀氀椀稀攀开挀漀渀渀攀挀琀椀漀渀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀            紀ഀ਀            攀氀猀攀ഀ਀            笀ഀ਀              眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀昀爀漀洀䈀礀琀攀猀匀攀挀 ⬀㴀 ⠀甀渀猀椀最渀攀搀⤀ 瀀欀琀开氀攀渀㬀ഀ਀              ⼀⼀ 圀漀爀欀 眀椀琀栀 椀琀⸀ഀ਀              昀漀爀 ⠀瀀欀琀开挀㴀　㬀瀀欀琀开挀㰀瀀欀琀开氀攀渀㬀瀀欀琀开挀⬀⬀⤀ഀ਀              笀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀戀甀昀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀爀攀愀搀⬀⬀崀 㴀 琀洀瀀爀挀瘀嬀瀀欀琀开挀崀㬀ഀ਀ഀ਀                椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀爀攀愀搀 㴀㴀 㐀⤀ഀ਀                笀ഀ਀                  ⼀⨀ 䐀攀挀爀礀瀀琀 琀栀攀 瀀愀挀欀攀琀 栀攀愀搀攀爀 愀昀琀攀爀 爀攀挀攀椀瘀椀渀最 㠀 戀礀琀攀猀⸀ ⨀⼀ഀ਀ഀ਀                  挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀氀椀攀渀琀开挀椀瀀栀攀爀㬀ഀ਀ഀ਀                  搀攀挀爀礀瀀琀挀漀瀀礀 ⠀ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀攀攀欀戀甀昀嬀　崀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀戀甀昀嬀　崀Ⰰ 㐀 ⤀㬀ഀ਀ഀ਀                  ⼀⨀ 䴀愀欀攀 猀甀爀攀 眀攀✀爀攀 攀砀瀀攀挀琀椀渀最 愀 洀甀氀琀椀瀀氀攀 漀昀 㠀 戀礀琀攀猀⸀ ⨀⼀ഀ਀ഀ਀                  洀攀洀挀瀀礀 ⠀ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀攀攀欀戀甀昀嬀　崀Ⰰ ㈀ ⤀㬀ഀ਀ഀ਀                  椀昀 ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 ─ 㐀 ⤀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 ⬀㴀 ⠀ 㐀 ⴀ ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 ─ 㐀 ⤀ ⤀㬀ഀ਀ഀ਀                  椀昀 ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 㸀 吀䌀倀开䈀唀䘀䘀䔀刀开匀䤀娀䔀 ⤀ഀ਀                  笀ഀ਀                    椀渀椀琀椀愀氀椀稀攀开挀漀渀渀攀挀琀椀漀渀 ⠀ 眀漀爀欀䌀漀渀渀攀挀琀 ⤀㬀ഀ਀                    戀爀攀愀欀㬀ഀ਀                  紀ഀ਀                紀ഀ਀ഀ਀                椀昀 ⠀ ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀爀攀愀搀 㴀㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 ⤀ ☀☀ ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 ℀㴀 　 ⤀ ⤀ഀ਀                笀ഀ਀                  椀昀 ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀 ⬀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 㸀 吀䌀倀开䈀唀䘀䘀䔀刀开匀䤀娀䔀 ⤀ഀ਀                  笀ഀ਀                    椀渀椀琀椀愀氀椀稀攀开挀漀渀渀攀挀琀椀漀渀 ⠀ 眀漀爀欀䌀漀渀渀攀挀琀 ⤀㬀ഀ਀                    戀爀攀愀欀㬀ഀ਀                  紀ഀ਀                  攀氀猀攀ഀ਀                  笀ഀ਀                    ⼀⨀ 䐀攀挀爀礀瀀琀 琀栀攀 爀攀猀琀 漀昀 琀栀攀 搀愀琀愀 椀昀 渀攀攀搀攀搀⸀ ⨀⼀ഀ਀ഀ਀                    挀椀瀀栀攀爀开瀀琀爀 㴀 ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀挀氀椀攀渀琀开挀椀瀀栀攀爀㬀ഀ਀ഀ਀                    洀攀洀挀瀀礀 ⠀ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀崀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀攀攀欀戀甀昀嬀　崀Ⰰ 㐀 ⤀㬀ഀ਀ഀ਀                    椀昀 ⠀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀爀攀愀搀 㸀 㐀 ⤀ഀ਀                      搀攀挀爀礀瀀琀挀漀瀀礀 ⠀ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀 ⬀ 㐀崀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀戀甀昀嬀㐀崀Ⰰ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀攀砀瀀攀挀琀 ⴀ 㐀 ⤀㬀ഀ਀ഀ਀                    洀攀洀挀瀀礀 ⠀ ☀琀栀椀猀开瀀愀挀欀攀琀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀攀攀欀戀甀昀嬀　崀Ⰰ ㈀ ⤀㬀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀搀愀琀愀 ⬀㴀 琀栀椀猀开瀀愀挀欀攀琀㬀ഀ਀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀愀挀欀攀琀猀匀攀挀 ⬀⬀㬀ഀ਀ഀ਀                    眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀爀挀瘀爀攀愀搀 㴀 　㬀ഀ਀                  紀ഀ਀                紀ഀ਀              紀ഀ਀            紀ഀ਀          紀ഀ਀ഀ਀          椀昀 ⠀䘀䐀开䤀匀匀䔀吀⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀圀爀椀琀攀䘀䐀猀⤀⤀ഀ਀          笀ഀ਀            ⼀⼀ 圀爀椀琀攀 猀栀椀琀ഀ਀ഀ਀            搀愀琀愀开猀攀渀搀 㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀搀愀琀愀 ⴀ 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀眀爀椀琀琀攀渀㬀ഀ਀ഀ਀            椀昀 ⠀⠀洀愀砀戀礀琀攀猀⤀ ☀☀ ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀攀渀搀椀渀最开昀椀氀攀猀⤀⤀ ⼀⼀ 圀攀 琀栀爀漀琀琀氀椀渀最㼀ഀ਀            笀ഀ਀              椀昀 ⠀ 渀甀洀开猀攀渀搀猀 ⤀ഀ਀                搀愀琀愀开猀攀渀搀 ⼀㴀 渀甀洀开猀攀渀搀猀㬀ഀ਀ഀ਀              椀昀 ⠀ 搀愀琀愀开猀攀渀搀 㸀 搀愀琀愀开爀攀洀愀椀渀椀渀最 ⤀ഀ਀                 搀愀琀愀开猀攀渀搀 㴀 搀愀琀愀开爀攀洀愀椀渀椀渀最㬀ഀ਀ഀ਀              椀昀 ⠀ 搀愀琀愀开猀攀渀搀 ⤀ഀ਀                搀愀琀愀开爀攀洀愀椀渀椀渀最 ⴀ㴀 搀愀琀愀开猀攀渀搀㬀ഀ਀            紀ഀ਀ഀ਀            椀昀 ⠀ 搀愀琀愀开猀攀渀搀 ⤀ഀ਀            笀ഀ਀              戀礀琀攀猀开猀攀渀琀 㴀 猀攀渀搀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀瀀氀礀匀漀挀欀昀搀Ⰰ ☀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀戀甀昀嬀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀眀爀椀琀琀攀渀崀Ⰰഀ਀                搀愀琀愀开猀攀渀搀Ⰰ 　⤀㬀ഀ਀              椀昀 ⠀戀礀琀攀猀开猀攀渀琀 㴀㴀 匀伀䌀䬀䔀吀开䔀刀刀伀刀⤀ഀ਀              笀ഀ਀                ⼀⨀ഀ਀                眀猀攀爀爀漀爀 㴀 圀匀䄀䜀攀琀䰀愀猀琀䔀爀爀漀爀⠀⤀㬀ഀ਀                瀀爀椀渀琀昀 ⠀∀䌀漀甀氀搀 渀漀琀 猀攀渀搀 搀愀琀愀 琀漀 挀氀椀攀渀琀⸀⸀⸀尀渀∀⤀㬀ഀ਀                瀀爀椀渀琀昀 ⠀∀匀漀挀欀攀琀 䔀爀爀漀爀 ─甀⸀尀渀∀Ⰰ 眀猀攀爀爀漀爀 ⤀㬀ഀ਀                ⨀⼀ഀ਀                椀渀椀琀椀愀氀椀稀攀开挀漀渀渀攀挀琀椀漀渀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀              紀ഀ਀              攀氀猀攀ഀ਀              笀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀眀爀椀琀琀攀渀 ⬀㴀 戀礀琀攀猀开猀攀渀琀㬀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀琀漀䈀礀琀攀猀匀攀挀 ⬀㴀 ⠀甀渀猀椀最渀攀搀⤀ 戀礀琀攀猀开猀攀渀琀㬀ഀ਀              紀ഀ਀ഀ਀              椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀眀爀椀琀琀攀渀 㴀㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀搀愀琀愀⤀ഀ਀                眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀眀爀椀琀琀攀渀 㴀 眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀猀渀搀搀愀琀愀 㴀 　㬀ഀ਀ഀ਀            紀ഀ਀          紀ഀ਀ഀ਀          椀昀 ⠀眀漀爀欀䌀漀渀渀攀挀琀ⴀ㸀琀漀搀挀⤀ഀ਀            椀渀椀琀椀愀氀椀稀攀开挀漀渀渀攀挀琀椀漀渀 ⠀眀漀爀欀䌀漀渀渀攀挀琀⤀㬀ഀ਀        紀ഀ਀      紀ഀ਀    紀ഀ਀  紀ഀ਀  爀攀琀甀爀渀 　㬀ഀ਀紀ഀ਀ഀ਀ഀ਀瘀漀椀搀 猀攀渀搀开琀漀开猀攀爀瘀攀爀⠀椀渀琀 猀漀挀欀Ⰰ 挀栀愀爀⨀ 瀀愀挀欀攀琀⤀ഀ਀笀ഀ਀ 椀渀琀 瀀欀琀氀攀渀㬀ഀ਀ഀ਀ 瀀欀琀氀攀渀 㴀 猀琀爀氀攀渀 ⠀瀀愀挀欀攀琀⤀㬀ഀ਀ഀ਀  椀昀 ⠀猀攀渀搀⠀猀漀挀欀Ⰰ 瀀愀挀欀攀琀Ⰰ 瀀欀琀氀攀渀Ⰰ 　⤀ ℀㴀 瀀欀琀氀攀渀⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀猀攀渀搀开琀漀开猀攀爀瘀攀爀⠀⤀㨀 昀愀椀氀甀爀攀∀⤀㬀ഀ਀    攀砀椀琀⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀紀ഀ਀ഀ਀椀渀琀 爀攀挀攀椀瘀攀开昀爀漀洀开猀攀爀瘀攀爀⠀椀渀琀 猀漀挀欀Ⰰ 挀栀愀爀⨀ 瀀愀挀欀攀琀⤀ഀ਀笀ഀ਀ 椀渀琀 瀀欀琀氀攀渀㬀ഀ਀ഀ਀  椀昀 ⠀⠀瀀欀琀氀攀渀 㴀 爀攀挀瘀⠀猀漀挀欀Ⰰ 瀀愀挀欀攀琀Ⰰ 吀䌀倀开䈀唀䘀䘀䔀刀开匀䤀娀䔀 ⴀ ㄀Ⰰ 　⤀⤀ 㰀㴀 　⤀ഀ਀  笀ഀ਀    瀀爀椀渀琀昀 ⠀∀爀攀挀攀椀瘀攀开昀爀漀洀开猀攀爀瘀攀爀⠀⤀㨀 昀愀椀氀甀爀攀∀⤀㬀ഀ਀    攀砀椀琀⠀㄀⤀㬀ഀ਀  紀ഀ਀  瀀愀挀欀攀琀嬀瀀欀琀氀攀渀崀 㴀 　㬀ഀ਀  爀攀琀甀爀渀 瀀欀琀氀攀渀㬀ഀ਀紀ഀ਀ഀ਀瘀漀椀搀 琀挀瀀开氀椀猀琀攀渀 ⠀椀渀琀 猀漀挀欀昀搀⤀ഀ਀笀ഀ਀  椀昀 ⠀氀椀猀琀攀渀⠀猀漀挀欀昀搀Ⰰ ㄀　⤀ 㰀 　⤀ഀ਀  笀ഀ਀    搀攀戀甀最开瀀攀爀爀漀爀 ⠀∀䌀漀甀氀搀 渀漀琀 氀椀猀琀攀渀 昀漀爀 挀漀渀渀攀挀琀椀漀渀∀⤀㬀ഀ਀    攀砀椀琀⠀㄀⤀㬀ഀ਀  紀ഀ਀紀ഀ਀ഀ਀椀渀琀 琀挀瀀开愀挀挀攀瀀琀 ⠀椀渀琀 猀漀挀欀昀搀Ⰰ 猀琀爀甀挀琀 猀漀挀欀愀搀搀爀 ⨀挀氀椀攀渀琀开愀搀搀爀Ⰰ 椀渀琀 ⨀愀搀搀爀开氀攀渀 ⤀ഀ਀笀ഀ਀  椀渀琀 昀搀㬀ഀ਀ഀ਀  椀昀 ⠀⠀昀搀 㴀 愀挀挀攀瀀琀 ⠀猀漀挀欀昀搀Ⰰ 挀氀椀攀渀琀开愀搀搀爀Ⰰ 愀搀搀爀开氀攀渀⤀⤀ 㰀 　⤀ഀ਀    搀攀戀甀最开瀀攀爀爀漀爀 ⠀∀䌀漀甀氀搀 渀漀琀 愀挀挀攀瀀琀 挀漀渀渀攀挀琀椀漀渀∀⤀㬀ഀ਀ഀ਀  爀攀琀甀爀渀 ⠀昀搀⤀㬀ഀ਀紀ഀ਀ഀ਀椀渀琀 琀挀瀀开猀漀挀欀开挀漀渀渀攀挀琀⠀挀栀愀爀⨀ 搀攀猀琀开愀搀搀爀Ⰰ 椀渀琀 瀀漀爀琀⤀ഀ਀笀ഀ਀  椀渀琀 昀搀㬀ഀ਀  猀琀爀甀挀琀 猀漀挀欀愀搀搀爀开椀渀 猀愀㬀ഀ਀ഀ਀  ⼀⨀ 䌀氀攀愀爀 椀琀 漀甀琀 ⨀⼀ഀ਀  洀攀洀猀攀琀⠀⠀瘀漀椀搀 ⨀⤀☀猀愀Ⰰ 　Ⰰ 猀椀稀攀漀昀⠀猀愀⤀⤀㬀ഀ਀ഀ਀  昀搀 㴀 猀漀挀欀攀琀⠀䄀䘀开䤀一䔀吀Ⰰ 匀伀䌀䬀开匀吀刀䔀䄀䴀Ⰰ 䤀倀倀刀伀吀伀开吀䌀倀⤀㬀ഀ਀ഀ਀  ⼀⨀ 䔀爀爀漀爀 ⨀⼀ഀ਀  椀昀⠀ 昀搀 㰀 　 ⤀ഀ਀    搀攀戀甀最开瀀攀爀爀漀爀⠀∀䌀漀甀氀搀 渀漀琀 挀爀攀愀琀攀 猀漀挀欀攀琀∀⤀㬀ഀ਀  攀氀猀攀ഀ਀  笀ഀ਀ഀ਀    洀攀洀猀攀琀 ⠀☀猀愀Ⰰ 　Ⰰ 猀椀稀攀漀昀⠀猀愀⤀⤀㬀ഀ਀    猀愀⸀猀椀渀开昀愀洀椀氀礀 㴀 䄀䘀开䤀一䔀吀㬀ഀ਀    猀愀⸀猀椀渀开愀搀搀爀⸀猀开愀搀搀爀 㴀 椀渀攀琀开愀搀搀爀 ⠀搀攀猀琀开愀搀搀爀⤀㬀ഀ਀    猀愀⸀猀椀渀开瀀漀爀琀 㴀 栀琀漀渀猀⠀⠀甀渀猀椀最渀攀搀 猀栀漀爀琀⤀ 瀀漀爀琀⤀㬀ഀ਀ഀ਀    椀昀 ⠀挀漀渀渀攀挀琀⠀昀搀Ⰰ ⠀猀琀爀甀挀琀 猀漀挀欀愀搀搀爀⨀⤀ ☀猀愀Ⰰ 猀椀稀攀漀昀⠀猀愀⤀⤀ 㰀 　⤀ഀ਀      搀攀戀甀最开瀀攀爀爀漀爀⠀∀䌀漀甀氀搀 渀漀琀 洀愀欀攀 吀䌀倀 挀漀渀渀攀挀琀椀漀渀∀⤀㬀ഀ਀    攀氀猀攀ഀ਀      搀攀戀甀最 ⠀∀琀挀瀀开猀漀挀欀开挀漀渀渀攀挀琀 ─猀㨀─甀∀Ⰰ 椀渀攀琀开渀琀漀愀 ⠀猀愀⸀猀椀渀开愀搀搀爀⤀Ⰰ 猀愀⸀猀椀渀开瀀漀爀琀 ⤀㬀ഀ਀  紀ഀ਀  爀攀琀甀爀渀⠀昀搀⤀㬀ഀ਀紀ഀ਀ഀ਀⼀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⼀ഀ਀椀渀琀 琀挀瀀开猀漀挀欀开漀瀀攀渀⠀猀琀爀甀挀琀 椀渀开愀搀搀爀 椀瀀Ⰰ 椀渀琀 瀀漀爀琀⤀ഀ਀笀ഀ਀  椀渀琀 昀搀Ⰰ 琀甀爀渀开漀渀开漀瀀琀椀漀渀开昀氀愀最 㴀 ㄀Ⰰ 爀挀匀漀挀欀漀瀀琀㬀ഀ਀ഀ਀  猀琀爀甀挀琀 猀漀挀欀愀搀搀爀开椀渀 猀愀㬀ഀ਀ഀ਀  ⼀⨀ 䌀氀攀愀爀 椀琀 漀甀琀 ⨀⼀ഀ਀  洀攀洀猀攀琀⠀⠀瘀漀椀搀 ⨀⤀☀猀愀Ⰰ 　Ⰰ 猀椀稀攀漀昀⠀猀愀⤀⤀㬀ഀ਀ഀ਀  昀搀 㴀 猀漀挀欀攀琀⠀倀䘀开䤀一䔀吀Ⰰ 匀伀䌀䬀开匀吀刀䔀䄀䴀Ⰰ 䤀倀倀刀伀吀伀开吀䌀倀⤀㬀ഀ਀ഀ਀  ⼀⨀ 䔀爀爀漀爀 ⨀⼀ഀ਀  椀昀⠀ 昀搀 㰀 　 ⤀笀ഀ਀    搀攀戀甀最开瀀攀爀爀漀爀⠀∀䌀漀甀氀搀 渀漀琀 挀爀攀愀琀攀 猀漀挀欀攀琀∀⤀㬀ഀ਀    攀砀椀琀⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀  猀愀⸀猀椀渀开昀愀洀椀氀礀 㴀 䄀䘀开䤀一䔀吀㬀ഀ਀  洀攀洀挀瀀礀⠀⠀瘀漀椀搀 ⨀⤀☀猀愀⸀猀椀渀开愀搀搀爀Ⰰ ⠀瘀漀椀搀 ⨀⤀☀椀瀀Ⰰ 猀椀稀攀漀昀⠀猀琀爀甀挀琀 椀渀开愀搀搀爀⤀⤀㬀ഀ਀  猀愀⸀猀椀渀开瀀漀爀琀 㴀 栀琀漀渀猀⠀⠀甀渀猀椀最渀攀搀 猀栀漀爀琀⤀ 瀀漀爀琀⤀㬀ഀ਀ഀ਀  ⼀⨀ 刀攀甀猀攀 瀀漀爀琀 ⠀䤀䌀匀㼀⤀ ⨀⼀ഀ਀ഀ਀  爀挀匀漀挀欀漀瀀琀 㴀 猀攀琀猀漀挀欀漀瀀琀⠀昀搀Ⰰ 匀伀䰀开匀伀䌀䬀䔀吀Ⰰ 匀伀开刀䔀唀匀䔀䄀䐀䐀刀Ⰰ ⠀挀栀愀爀 ⨀⤀ ☀琀甀爀渀开漀渀开漀瀀琀椀漀渀开昀氀愀最Ⰰ 猀椀稀攀漀昀⠀琀甀爀渀开漀渀开漀瀀琀椀漀渀开昀氀愀最⤀⤀㬀ഀ਀ഀ਀  ⼀⨀ 戀椀渀搀⠀⤀ 琀栀攀 猀漀挀欀攀琀 琀漀 琀栀攀 椀渀琀攀爀昀愀挀攀 ⨀⼀ഀ਀  椀昀 ⠀戀椀渀搀⠀昀搀Ⰰ ⠀猀琀爀甀挀琀 猀漀挀欀愀搀搀爀 ⨀⤀☀猀愀Ⰰ 猀椀稀攀漀昀⠀猀琀爀甀挀琀 猀漀挀欀愀搀搀爀⤀⤀ 㰀 　⤀笀ഀ਀    搀攀戀甀最开瀀攀爀爀漀爀⠀∀䌀漀甀氀搀 渀漀琀 戀椀渀搀 琀漀 瀀漀爀琀∀⤀㬀ഀ਀    攀砀椀琀⠀㄀⤀㬀ഀ਀  紀ഀ਀ഀ਀  爀攀琀甀爀渀⠀昀搀⤀㬀ഀ਀紀ഀ਀ഀ਀⼀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀ഀ਀⨀ 猀愀洀攀 愀猀 搀攀戀甀最开瀀攀爀爀漀爀 戀甀琀 眀爀椀琀攀猀 琀漀 搀攀戀甀最 漀甀琀瀀甀琀⸀ഀ਀⨀ഀ਀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⼀ഀ਀瘀漀椀搀 搀攀戀甀最开瀀攀爀爀漀爀⠀ 挀栀愀爀 ⨀ 洀猀最 ⤀ 笀ഀ਀  搀攀戀甀最⠀ ∀─猀 㨀 ─猀尀渀∀ Ⰰ 洀猀最 Ⰰ 猀琀爀攀爀爀漀爀⠀攀爀爀渀漀⤀ ⤀㬀ഀ਀紀ഀ਀⼀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⨀⼀ഀ਀瘀漀椀搀 搀攀戀甀最⠀挀栀愀爀 ⨀昀洀琀Ⰰ ⸀⸀⸀⤀ഀ਀笀ഀ਀⌀搀攀昀椀渀攀 䴀䄀堀开䴀䔀匀䜀开䰀䔀一 ㄀　㈀㐀ഀ਀ഀ਀  瘀愀开氀椀猀琀 愀爀最猀㬀ഀ਀  挀栀愀爀 琀攀砀琀嬀 䴀䄀堀开䴀䔀匀䜀开䰀䔀一 崀㬀ഀ਀ഀ਀  瘀愀开猀琀愀爀琀 ⠀愀爀最猀Ⰰ 昀洀琀⤀㬀ഀ਀  猀琀爀挀瀀礀 ⠀琀攀砀琀 ⬀ 瘀猀瀀爀椀渀琀昀⠀ 琀攀砀琀Ⰰ昀洀琀Ⰰ愀爀最猀⤀Ⰰ ∀尀爀尀渀∀⤀㬀ഀ਀  瘀愀开攀渀搀 ⠀愀爀最猀⤀㬀ഀ਀ഀ਀  昀瀀爀椀渀琀昀⠀ 猀琀搀攀爀爀Ⰰ ∀─猀∀Ⰰ 琀攀砀琀⤀㬀ഀ਀紀ഀ਀ഀ਀ഀ਀瘀漀椀搀 䌀刀夀倀吀开倀䌀开䴀椀砀䬀攀礀猀⠀䌀刀夀倀吀开匀䔀吀唀倀⨀ 瀀挀⤀ഀ਀笀ഀ਀    甀渀猀椀最渀攀搀 氀漀渀最 攀猀椀Ⰰ攀搀椀Ⰰ攀愀砀Ⰰ攀戀瀀Ⰰ攀搀砀㬀ഀ਀    攀搀椀 㴀 ㄀㬀ഀ਀    攀搀砀 㴀 　砀㄀㠀㬀ഀ਀    攀愀砀 㴀 攀搀椀㬀ഀ਀    眀栀椀氀攀 ⠀攀搀砀 㸀 　⤀ഀ਀    笀ഀ਀        攀猀椀 㴀 瀀挀ⴀ㸀欀攀礀猀嬀攀愀砀 ⬀ 　砀㄀䘀崀㬀ഀ਀        攀戀瀀 㴀 瀀挀ⴀ㸀欀攀礀猀嬀攀愀砀崀㬀ഀ਀        攀戀瀀 㴀 攀戀瀀 ⴀ 攀猀椀㬀ഀ਀        瀀挀ⴀ㸀欀攀礀猀嬀攀愀砀崀 㴀 攀戀瀀㬀ഀ਀        攀愀砀⬀⬀㬀ഀ਀        攀搀砀ⴀⴀ㬀ഀ਀    紀ഀ਀    攀搀椀 㴀 　砀㄀㤀㬀ഀ਀    攀搀砀 㴀 　砀㄀䘀㬀ഀ਀    攀愀砀 㴀 攀搀椀㬀ഀ਀    眀栀椀氀攀 ⠀攀搀砀 㸀 　⤀ഀ਀    笀ഀ਀        攀猀椀 㴀 瀀挀ⴀ㸀欀攀礀猀嬀攀愀砀 ⴀ 　砀㄀㠀崀㬀ഀ਀        攀戀瀀 㴀 瀀挀ⴀ㸀欀攀礀猀嬀攀愀砀崀㬀ഀ਀        攀戀瀀 㴀 攀戀瀀 ⴀ 攀猀椀㬀ഀ਀        瀀挀ⴀ㸀欀攀礀猀嬀攀愀砀崀 㴀 攀戀瀀㬀ഀ਀        攀愀砀⬀⬀㬀ഀ਀        攀搀砀ⴀⴀ㬀ഀ਀    紀ഀ਀紀ഀ਀ഀ਀瘀漀椀搀 䌀刀夀倀吀开倀䌀开䌀爀攀愀琀攀䬀攀礀猀⠀䌀刀夀倀吀开匀䔀吀唀倀⨀ 瀀挀Ⰰ甀渀猀椀最渀攀搀 氀漀渀最 瘀愀氀⤀ഀ਀笀ഀ਀    甀渀猀椀最渀攀搀 氀漀渀最 攀猀椀Ⰰ攀戀砀Ⰰ攀搀椀Ⰰ攀愀砀Ⰰ攀搀砀Ⰰ瘀愀爀㄀㬀ഀ਀    攀猀椀 㴀 ㄀㬀ഀ਀    攀戀砀 㴀 瘀愀氀㬀ഀ਀    攀搀椀 㴀 　砀㄀㔀㬀ഀ਀    瀀挀ⴀ㸀欀攀礀猀嬀㔀㘀崀 㴀 攀戀砀㬀ഀ਀    瀀挀ⴀ㸀欀攀礀猀嬀㔀㔀崀 㴀 攀戀砀㬀ഀ਀    眀栀椀氀攀 ⠀攀搀椀 㰀㴀 　砀㐀㘀䔀⤀ഀ਀    笀ഀ਀        攀愀砀 㴀 攀搀椀㬀ഀ਀        瘀愀爀㄀ 㴀 攀愀砀 ⼀ 㔀㔀㬀ഀ਀        攀搀砀 㴀 攀愀砀 ⴀ ⠀瘀愀爀㄀ ⨀ 㔀㔀⤀㬀ഀ਀        攀戀砀 㴀 攀戀砀 ⴀ 攀猀椀㬀ഀ਀        攀搀椀 㴀 攀搀椀 ⬀ 　砀㄀㔀㬀ഀ਀        瀀挀ⴀ㸀欀攀礀猀嬀攀搀砀崀 㴀 攀猀椀㬀ഀ਀        攀猀椀 㴀 攀戀砀㬀ഀ਀        攀戀砀 㴀 瀀挀ⴀ㸀欀攀礀猀嬀攀搀砀崀㬀ഀ਀    紀ഀ਀    䌀刀夀倀吀开倀䌀开䴀椀砀䬀攀礀猀⠀瀀挀⤀㬀ഀ਀    䌀刀夀倀吀开倀䌀开䴀椀砀䬀攀礀猀⠀瀀挀⤀㬀ഀ਀    䌀刀夀倀吀开倀䌀开䴀椀砀䬀攀礀猀⠀瀀挀⤀㬀ഀ਀    䌀刀夀倀吀开倀䌀开䴀椀砀䬀攀礀猀⠀瀀挀⤀㬀ഀ਀    瀀挀ⴀ㸀瀀挀开瀀漀猀渀 㴀 㔀㘀㬀ഀ਀紀ഀ਀ഀ਀甀渀猀椀最渀攀搀 氀漀渀最 䌀刀夀倀吀开倀䌀开䜀攀琀一攀砀琀䬀攀礀⠀䌀刀夀倀吀开匀䔀吀唀倀⨀ 瀀挀⤀ഀ਀笀ഀ਀    甀渀猀椀最渀攀搀 氀漀渀最 爀攀㬀ഀ਀    椀昀 ⠀瀀挀ⴀ㸀瀀挀开瀀漀猀渀 㴀㴀 㔀㘀⤀ഀ਀    笀ഀ਀        䌀刀夀倀吀开倀䌀开䴀椀砀䬀攀礀猀⠀瀀挀⤀㬀ഀ਀        瀀挀ⴀ㸀瀀挀开瀀漀猀渀 㴀 ㄀㬀ഀ਀    紀ഀ਀    爀攀 㴀 瀀挀ⴀ㸀欀攀礀猀嬀瀀挀ⴀ㸀瀀挀开瀀漀猀渀崀㬀ഀ਀    瀀挀ⴀ㸀瀀挀开瀀漀猀渀⬀⬀㬀ഀ਀    爀攀琀甀爀渀 爀攀㬀ഀ਀紀ഀ਀ഀ਀瘀漀椀搀 䌀刀夀倀吀开倀䌀开䌀爀礀瀀琀䐀愀琀愀⠀䌀刀夀倀吀开匀䔀吀唀倀⨀ 瀀挀Ⰰ瘀漀椀搀⨀ 搀愀琀愀Ⰰ甀渀猀椀最渀攀搀 氀漀渀最 猀椀稀攀⤀ഀ਀笀ഀ਀    甀渀猀椀最渀攀搀 氀漀渀最 砀㬀ഀ਀    昀漀爀 ⠀砀 㴀 　㬀 砀 㰀 猀椀稀攀㬀 砀 ⬀㴀 㐀⤀ ⨀⠀甀渀猀椀最渀攀搀 氀漀渀最⨀⤀⠀⠀甀渀猀椀最渀攀搀 氀漀渀最⤀搀愀琀愀 ⬀ 砀⤀ 帀㴀 䌀刀夀倀吀开倀䌀开䜀攀琀一攀砀琀䬀攀礀⠀瀀挀⤀㬀ഀ਀紀ഀ਀ഀ਀瘀漀椀搀 搀攀挀爀礀瀀琀挀漀瀀礀 ⠀ 瘀漀椀搀⨀ 搀攀猀琀Ⰰ 瘀漀椀搀⨀ 猀漀甀爀挀攀Ⰰ 甀渀猀椀最渀攀搀 猀椀稀攀 ⤀ഀ਀笀ഀ਀  䌀刀夀倀吀开倀䌀开䌀爀礀瀀琀䐀愀琀愀⠀挀椀瀀栀攀爀开瀀琀爀Ⰰ猀漀甀爀挀攀Ⰰ猀椀稀攀⤀㬀ഀ਀  洀攀洀挀瀀礀 ⠀搀攀猀琀Ⰰ猀漀甀爀挀攀Ⰰ猀椀稀攀⤀㬀ഀ਀紀ഀ਀ഀ਀瘀漀椀搀 攀渀挀爀礀瀀琀挀漀瀀礀 ⠀ 䈀䄀一䄀一䄀⨀ 挀氀椀攀渀琀Ⰰ 瘀漀椀搀⨀ 猀漀甀爀挀攀Ⰰ 甀渀猀椀最渀攀搀 猀椀稀攀 ⤀ഀ਀笀ഀ਀  甀渀猀椀最渀攀搀 挀栀愀爀⨀ 搀攀猀琀㬀ഀ਀ഀ਀  椀昀 ⠀吀䌀倀开䈀唀䘀䘀䔀刀开匀䤀娀䔀 ⴀ 挀氀椀攀渀琀ⴀ㸀猀渀搀搀愀琀愀 㰀 ⠀ ⠀椀渀琀⤀ 猀椀稀攀 ⬀ 㜀 ⤀ ⤀ഀ਀    挀氀椀攀渀琀ⴀ㸀琀漀搀挀 㴀 ㄀㬀ഀ਀  攀氀猀攀ഀ਀  笀ഀ਀    搀攀猀琀 㴀 ☀挀氀椀攀渀琀ⴀ㸀猀渀搀戀甀昀嬀挀氀椀攀渀琀ⴀ㸀猀渀搀搀愀琀愀崀㬀ഀ਀    洀攀洀挀瀀礀 ⠀搀攀猀琀Ⰰ猀漀甀爀挀攀Ⰰ猀椀稀攀⤀㬀ഀ਀    眀栀椀氀攀 ⠀猀椀稀攀 ─ 㐀⤀ഀ਀      搀攀猀琀嬀猀椀稀攀⬀⬀崀 㴀 　砀　　㬀ഀ਀    挀氀椀攀渀琀ⴀ㸀猀渀搀搀愀琀愀 ⬀㴀 ⠀椀渀琀⤀ 猀椀稀攀㬀ഀ਀ഀ਀    䌀刀夀倀吀开倀䌀开䌀爀礀瀀琀䐀愀琀愀⠀挀椀瀀栀攀爀开瀀琀爀Ⰰ搀攀猀琀Ⰰ猀椀稀攀⤀㬀ഀ਀  紀ഀ਀紀ഀ਀\00