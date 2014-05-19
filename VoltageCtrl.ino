/*
 Web Server

 A simple web server that process the URL for a string with command to set voltages.

 created 1 April 2014
 by Masaru Natsu
*/
// Do not remove the include below
#include <SPI.h>
#include <Ethernet.h>

#define AEDUINOMEGA (1)
/***************************************************************************
                          CONFIGURATION
****************************************************************************/
#if ARDUINOMEGA
#define myMAC      {0xDE,0xAD,0xBE,0xEF,0xFE,0xF2 }
#define myIP       10,246,190,158
#else
// local macros, configuration changes go here!!
#define myMAC      {0xCA,0xFE,0xC0,0xC0,0xBE,0xBE }
#define myIP       172,23,20,70
#endif
#define myGateway  172,23,20,1
#define mySubNet   255,255,255,0
#define myDNS      10,252,192,50

#define RxQUEUESIZE (21)
#define RS485_DE    (11)
#define MYUBRR      (51)

/***************************************************************************
                        HANDY TOOLS
****************************************************************************/
#define GETHIGHBYTE(value) ((unsigned char)(value >> 8))
#define GETLOWBYTE(value)  ((unsigned char)(value))

#define GETHIGHBYTE(value) ((unsigned char)(value >> 8))
#define GETLOWBYTE(value)  ((unsigned char)(value))

#define ENABLETxRx  (UCSR0B = (1 << RXEN0) | (1 << TXEN0))
#define ENABLETx    (UCSR0B = (1 << TXEN0))
#define ENABLERx    (UCSR0B = (1 << RXEN0))
#define DISABLETx   (UCSR0B = (0 << TXEN0))
#define DISABLERx   (UCSR0B = (0 << RXEN0))

#define GETNEXT(index)  ((index + 1) % RxQUEUESIZE)

/***************************************************************************
                          LOCAL TYPES
****************************************************************************/
typedef uint8_t tByte;
typedef tByte tDstAddr;
typedef tByte tSrcAddr;
typedef tByte tLength;
typedef tByte tCommand;
typedef tByte tChannel;
typedef tByte tMSByte;
typedef tByte tLSByte;

typedef uint16_t tData;

typedef enum {Enable   = 0x03,\
              Disable  = 0x04,\
              Reset    = 0x05,\
              Set      = 0x06,\
              Activate = 0x07,\
              Symmetry = 0x08,\
              NoData   = 0xFF};

typedef struct tPayload
{
  tMSByte msByte;
  tLSByte lsByte;
};

typedef struct tPacket
{
  tDstAddr dstAddr;
  tSrcAddr srcAddr;
  tLength  length;
  tCommand command;
  tChannel channel;
  tPayload payload;
};

unsigned int iHead;
unsigned int iTail;
unsigned int iRxQueue[RxQUEUESIZE];

/***************************************************************************
                    DIGITAL INPUT DEFINITIONS
****************************************************************************/
// Digital inputs configuration
#define Bk1Max  9
#define Bk1Off  2
#define Bk1Dlt  1
#define Bk2Max  38
#define Bk2Off  24
#define Bk2Dlt  2


/***************************************************************************
                         GLOBAL DECALARATIONS
****************************************************************************/
// the local prototypes
void processHTTPRequest(void);
void serverInfo(EthernetClient client, String myClientData);
void processDataString();
void buildPacket(tCommand commandID, tPacket *pPacket, boolean includePayload, String currCmd);
void USART_Init(unsigned int ubrr);
void USART_Tx(unsigned int data);
void Push(unsigned int iData);
unsigned int Pop(void);
void HighLow_RS485DE(boolean status);
boolean isRxQueueEmpty(void);
// Capacitor banks prototypes
String getDBank(int BkOff, int BkMax, int BkDlt);

// Global collected data to be processed.
String myData = "";

// Enter a MAC address and IP address for your controller below.
// The IP address will be dependent on your local network:
byte mac[] = myMAC;
IPAddress ip(myIP);
IPAddress gateway(myGateway);
IPAddress subNet(mySubNet);
IPAddress dnServer(myDNS);

// Initialize the Ethernet server library
// with the IP address and port you want to use
// (port 80 is default for HTTP):
EthernetServer server(80);

/***************************************************************************
                      ARDUINO SPECIFIC CODE
****************************************************************************/

//The setup function is called once at startup of the sketch
void setup()
{
  // Add your initialization code here
  USART_Init(MYUBRR);
  iHead = 0;
  iTail = 0;

  // start the Ethernet connection and the server:
#if ARDUINOMEGA
  Ethernet.begin(mac, ip);
#else
  Ethernet.begin(mac, ip, dnServer, gateway, subNet);
#endif
  server.begin();

  { // Setup the digital input bank
    int i;

    // Bank #1
    for (i = Bk1Off; i < Bk1Max; i += Bk1Dlt) { pinMode(i, INPUT); }

    // Bank #2
    for (i = Bk2Off; i < Bk2Max; i += Bk2Dlt) { pinMode(i, INPUT); }
  }

  interrupts();
}

// The loop function is called in an endless loop
void loop()
{
  processHTTPRequest();
  processDataString();
}

/***************************************************************************
                      APPLICATION'S SPECIFIC CODE
****************************************************************************/
//
// This function sends the client header and data back, so the client gets the needed data
// and to avoid having a client that times out.
void serverInfo(EthernetClient client, String myClientData)
{
  // send a standard http response header
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");  // the connection will be closed after completion of the response
  client.println("Refresh: 5");  // refresh the page automatically every 5 sec
  client.println();
  client.println("<!DOCTYPE HTML>");
  client.println("<html>");
  client.println("The data: ");  // send back the data to the client fo no reason...
  client.println(myClientData);
  client.println("</html>");
}

/***************************************************************************
                      CLIENT'S REQUEST PROCESSOR
****************************************************************************/
//
// This function process the client connection, basically the URL connection might have the data to process.
// the data to be processed are the actual commands to be send to  Jim's magical board.
// YES the baord is magical.
//
// HTTP request examples:
//  SET:
//     http://<IP ADDRESS>/?s6_2_255z
//  ENABLE:
//     http://<IP ADDRESS>/?e2z
//  DISABLE:
//     http://<IP ADDRESS>/?d5z
//  RESET:
//     http://<IP ADDRESS>/?r3z
//  ACTIVATE:
//     http://<IP ADDRESS>/?a6z
//  ALL TOGETHER:
//     http://<IP ADDRESS>/?s6_2_255ze2zd5zr3za6z
void processHTTPRequest()
{
  // listen for incoming clients
  EthernetClient client = server.available();

  // keep the current data mode, we wont be porcessing until an specific condition is has been met.
  boolean isReading = false;

  // keep the results for the digital inputs
  String digitalInputs;

  // The whole set of data, start fresh every time.
  myData = "";

  // if there is no client available there is nothing to process.
  if (!client) { return; } // quit as soon as possible to be able to porcess the next request sooner.

  // While a valid client connectionis present .. then porcess the commands if any.
  while (client.connected())
  {
    // check if the client connected is actually available.
    if (!client.available()) { continue; } // if the client is not ready keep checking, don't porcess anything else.

    // get 1 character at the time form the http url, this will process the end of line as soon is found and we don't know how big the string is.
    char c = client.read();

    // check if we are at the start of the input string, we only read characters after this sign.
    // we don't care how long the URL is as soon this question mark is found we asume the rest to be the string with the commands.
    if (c == '?')
    {
      // change the mode be in Reading mode
      isReading = true;
      continue;
    }

    // if we are in reading mode and we have reached the end of the line or we have reached this invalid character, this  means something went wrong stop reading.
    if (isReading && (c == ' '))
    {
      isReading = false;
      continue; // WE can make this process faster by setting this to a break intruction and leave the while loop at this point.
    }

    // Check if we have reached the end of the line.. if so prepare to end this process.
    if (c == '\r')
    {
      isReading = false;
      continue;
    }

    // if we reach the end of the line and the character is a new line, then we are done processing and reading.
    if (c == '\n')
    {
      // end the connection with the client as soon as possible to aovid timeout.
      serverInfo(client, digitalInputs);
      // quit the loop...
      break;
    }

    // Collect all the data in the input string!!
    if (isReading) { myData += c; }

    // Special case for the digitial inputs the data will be displayed in to the client inserted in the same command string
    if (c == 'b')
    {
      digitalInputs = getDBank(Bk1Off, Bk1Max, Bk1Dlt);
      digitalInputs += "::";
      digitalInputs += getDBank(Bk2Off, Bk2Max, Bk2Dlt);
    }
  } // End of while loop

  // give the web browser time to receive the data
  delay(3);

  // close the connection:
  client.stop();
}

/***************************************************************************
                            COMMAND PARSER
****************************************************************************/
//
// This function will process the data string, build the command and send them over the serial port to the master board.
void processDataString()
{
  // The index for the substrings to process and end-of-string locator.
  int stringIndex = 0;

  // The last index of the current command bing processed.
  int endOfCommand;

  // The command to be send out
  tPacket myCommand;

  //
  tCommand cmdID;

  // Skip processing the empty string.
  if (!myData.length()) { return; }

  // while we haven't reached the end of the string keep processing.
  while (stringIndex < myData.length())
  {
    // estimate the end index of the command to process.
    endOfCommand = myData.indexOf("z", stringIndex);

    // If we are unable to estimate the end of the current command to process the string is corrupted, end the processing.
    if (endOfCommand < 0) { return; }

    // get the substring with the current command to process.
    String currentCmd = myData.substring(stringIndex, endOfCommand);

    // move the index and have it ready for the next command.
    stringIndex = endOfCommand + 1;  //  endOfCommad is in the index number matching the last character the "z", we need to be one character right after that.

    // it needs at least 1 digit for the channel + 'e' + 1 digit for the value = 3 Characters
    if (currentCmd.length() < 2) { continue; }

    // print means send it over the serial port to the master board.
    boolean readyToPrint = true;

    // The first character tells us the type of command to process.
    char cmd = currentCmd[0];

    //
    boolean needPayload = false;

    // select the proper command
    switch (cmd)
    {
      // SET: Sets the holding register value for the channel.
      // Format:
      //     set<space><board addr><space><channel number><space><Value 1 to 1000><CR>
      case 's':
        // if the set command doesn't have the minimum amount of characters then is an invalid set command.
        if (currentCmd.length() < 5)
        {
          // Don't send this command out and move to the next command.
          readyToPrint = false;
          break;
        }
        needPayload = true;
        cmdID = Set;
      break;

      // ENABLE: Take output amps oout of the standby mode
      // Format:
      //     enable<space><board addr><CR>
      case 'e':
        // Build the command to be send out.. very simple..
        cmdID = Enable;
      break;

      // DISABLE: Put output amps in stand by mode
      // Format:
      //     disable<space><board addr><CR>
      case 'd':
        cmdID = Disable;
      break;

      // RESET: Sets all channels to neutral value.
      // Format:
      //     reset<space><board addr><CR>
      case 'r':
        cmdID = Reset;
      break;

      // ACTIVATE: transfer holding registers to outputs on all boards and channels.
      // Format:
      //     activate<space><board addr><CR>
      case 'a': // broadcast command
        cmdID = Activate;
      break;

      // Any other command will be excluded.
      default:
        readyToPrint = false;
    } // End of switch

    // if the command was built from the http string then we will send it over the serial port.
    if (readyToPrint)
    {
      buildPacket(cmdID, &myCommand, needPayload, currentCmd);
      // here we put the whole command together with <CR> at the end.
      HighLow_RS485DE(HIGH);
      USART_Tx((tData)(0x0100 | myCommand.dstAddr));
      USART_Tx((tData)myCommand.srcAddr);
      USART_Tx((tData)myCommand.length);
      USART_Tx((tData)myCommand.command);
      USART_Tx((tData)myCommand.channel);

      if (myCommand.command == Set)
      {
        USART_Tx((tData)myCommand.payload.msByte);
        USART_Tx((tData)myCommand.payload.lsByte);
      }
      HighLow_RS485DE(LOW);
      delay(500);
      needPayload = false;
    }

  } // End of while
}


//
void buildPacket(tCommand commandID, tPacket *pPacket, boolean includePayload, String currCmd)
{
  // Build the command to be send out.. very simple..
  int currIndx = 1;
  int endIndx = currCmd.length();
  String data;

  pPacket->command = commandID;
  pPacket->srcAddr = NoData;
  pPacket->length = (includePayload)? sizeof(tPacket) : sizeof(tPacket) - sizeof(tPayload);
    
  // if the command only uses the channel and has no extra payload then...
  endIndx =  (commandID != Set)? currCmd.indexOf("z") : currCmd.indexOf("_");
  
  // This is the address of the board that will receive the command
  data = currCmd.substring(currIndx, endIndx);
  pPacket->dstAddr = data.toInt();
  if (commandID != Set) 
  { 
    pPacket->channel = NoData;
    return; 
  }

  // This is the channel or sub-board to address...
  currIndx = endIndx + 1;
  endIndx = currCmd.indexOf("_", currIndx);
  data = currCmd.substring(currIndx, endIndx);
  pPacket->channel = data.toInt();

  //  if is the set command we have extra info..!
  if (!includePayload) { return; }

  currIndx = endIndx + 1;
  data = currCmd.substring(currIndx, currCmd.length());
  pPacket->payload.msByte = (tMSByte)(data.toInt() >> 8);
  pPacket->payload.lsByte = (tLSByte)data.toInt();
}

/***************************************************************************
                         Serial Init and Tx
****************************************************************************/
// Configures the serial port and enables the 9 data bit sna dn 2 stop bits.
void USART_Init(unsigned int ubrr)
{
  // set the baud rate

  //UBRR0H = 0;
  //UBRR0L = 51;
  UBRR0H = GETHIGHBYTE(ubrr);
  UBRR0L = GETLOWBYTE(ubrr);

  // Enable the receiver and the transmitter
  ENABLETxRx;

  // Set frame format: 9data, 2stop bit
  UCSR0C = ((1 << USBS0) | (3 << UCSZ00));
  UCSR0B |= ( 1 << UCSZ02 );

  // configure the pin to drive the Tx on the RS485
  pinMode(RS485_DE, OUTPUT);
}


void USART_Tx(unsigned int data)
{
  noInterrupts();

  // wait for the Tx buffer to be empty
  while (!(UCSR0A & (1 << UDRE0)));

  // set the 9th bit if needed
  UCSR0B &= ~(1 << TXB80);
  if (data & 0x0100) { UCSR0B |= (1 << TXB80); }

  // write the data register.
  UDR0 = data;
  delay(2);
  interrupts();
}

unsigned int USART_Rx(void)
{
  unsigned char Status, resh, resl;

  /* Wait for data to be received */
  while (!(UCSR0A & (1 << RXC0)));

  /* Get status and 9th bit, then data */

  /* from buffer */
  Status = UCSR0A;
  resh = UCSR0B;
  resl = UDR0;

  /* If error, return -1 */
  if ((Status & (1 << FE0)) | (1 << DOR0) | (1 << UPE0)) { return -1; }


  /* Filter the 9th bit, then return */
  resh = (resh >> 1) & 0x01;
  return ((resh << 8) | resl);
}

ISR(USART0_RX_vect)
{
	noInterrupts();
	unsigned int rxData = USART_Rx();

	if (rxData != -1) { Push(rxData); }
	interrupts();
}

void Push(unsigned int iData)
{
	// Don't push more data if the queue is full
	if (GETNEXT(iHead) == iTail) { return; }

	iHead = GETNEXT(iHead);

	iRxQueue[iHead] = ((unsigned int)iData);
}

unsigned int Pop(void)
{
	//  if the queue is empty, fail the pop..
	if (iHead == iTail) { return -1; }

	return (iRxQueue[iTail++]);
}

boolean isRxQueueEmpty()
{
	return (iHead == iTail)? true : false;
}

void HighLow_RS485DE(boolean status)
{
	while (!(UCSR0A & (1 << UDRE0)));

	// Set the RS485DE high to be able to transmit
	delay(1);
	digitalWrite(RS485_DE, status);
	delay(1);
}

/***************************************************************************
                         DIGITAL INPUTS READER
****************************************************************************/
//
// Gets the digital values for a given bank of input and returns the string version of the results.
String getDBank(int BkOff, int BkMax, int BkDlt)
{
  String results = ""; //  string to hold the 1 and 0's as result, for each ppin in the digital inputs.
  int pin;

  for (pin = BkOff; pin < BkMax; pin += BkDlt)
  {
    // read the digitail input
    int pinState = digitalRead(pin);
    // This lines needs no explanation ...
    results += (pinState == HIGH)? "1" : "0";
  }

  return results;
}

