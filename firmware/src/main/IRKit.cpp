#include "pins.h"
#include "MemoryFree.h"
#include "pgmStrToRAM.h"
#include "IrCtrl.h"
#include "FullColorLed.h"
#include "version.h"
#include "GSwifi.h"
#include "Keys.h"
#include "FlexiTimer2.h"
#include "Global.h"
#include "MorseListener.h"
#include "IrJsonParser.h"
#include "timer.h"
#include "LongPressButton.h"
#include "base64encoder.h"

// Serial1(RX=D0,TX=D1) is Wifi module's UART interface
static GSwifi gs(&Serial1X);
static FullColorLed color( FULLCOLOR_LED_R, FULLCOLOR_LED_G, FULLCOLOR_LED_B );
static MorseListener listener(MICROPHONE,100);
static LongPressButton clear_button(RESET_SWITCH, 5);
static Keys keys;
volatile static uint8_t message_timer         = TIMER_OFF;
volatile static uint8_t reconnect_timer       = TIMER_OFF;

static uint32_t newest_message_id = 0; // on memory only should be fine
static bool     morse_error       = 0;
static uint8_t  post_keys_cid;

#define COMMAND_POST_KEYS  1
#define COMMAND_SETUP      2
#define COMMAND_CONNECT    3
#define COMMAND_CLOSE      4
#define COMMAND_START      5

#define COMMAND_QUEUE_SIZE 6
static struct RingBuffer command_queue;
static char command_queue_data[COMMAND_QUEUE_SIZE + 1];

#define POST_DOOR_BODY_LENGTH 61
#define POST_KEYS_BODY_LENGTH 42

//--- declaration

void   reset3V3();
void   longPressed();
void   timerLoop();
void   onIRReceive();
void   onTimer();
int8_t onReset();
int8_t onDisconnect();
void   jsonDetectedStart();
void   jsonDetectedData( uint8_t key, uint16_t value );
void   jsonDetectedEnd();
void   onIRXmitComplete();
int8_t onPostMessagesRequest(uint8_t cid, GSwifi::GSREQUESTSTATE state);
int8_t onPostKeysRequest(uint8_t cid, GSwifi::GSREQUESTSTATE state);
int8_t onRequest(uint8_t cid, int8_t routeid, GSwifi::GSREQUESTSTATE state);
int8_t onPostDoorResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state);
int8_t onGetMessagesResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state);
int8_t onPostKeysResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state);
void   postDoor();
int8_t getMessages();
int8_t postMessages();
int8_t postKeys();
void   connect();
void   startNormalOperation();
void   letterCallback( char letter );
void   wordCallback();
void   IRKit_setup();
void   IRKit_loop();

//--- implementation

void reset3V3 () {
    Serial.println(("!E25"));

    gs.reset();

    digitalWrite( LDO33_ENABLE, LOW );
    delay( 3000 );
    digitalWrite( LDO33_ENABLE, HIGH );

    // wait til gs wakes up
    delay( 1000 );
}

void longPressed() {
    Serial.println("long");
    keys.clear();
    keys.save();
    reset3V3();
}

void timerLoop() {
    // long poll
    if (TIMER_FIRED(message_timer)) {
        TIMER_STOP(message_timer);
        int8_t result = getMessages();
        if ( result != 0 ) {
            // Serial.println(("!E3"));
            // maybe time cures GS?
            TIMER_START(message_timer, 5);
            // reset if any error happens
            // reset3V3();
        }
    }

    // reconnect
    if (TIMER_FIRED(reconnect_timer)) {
        TIMER_STOP(reconnect_timer);
        connect();
    }

    while (! ring_isempty(&command_queue)) {
        char command;
        ring_get(&command_queue, &command, 1);

        switch (command) {
        case COMMAND_POST_KEYS:
            {
                int8_t result = postKeys();
                if ( result < 0 ) {
                    gs.writeHead( post_keys_cid, 500 );
                    gs.writeEnd();
                    gs.close( post_keys_cid );
                }
            }
            break;
        case COMMAND_SETUP:
            gs.setup( &onDisconnect, &onReset );
            // vv continues
        case COMMAND_CONNECT:
            connect();
            break;
        case COMMAND_CLOSE:
            ring_get(&command_queue, &command, 1);
            gs.close(command);
            break;
        case COMMAND_START:
            startNormalOperation();
            break;
        default:
            break;
        }
    }
}

void onIRReceive() {
    IR_dump();
    if (IR_packedlength() > 0) {
        postMessages();
    }
}

// inside ISR, be careful
void onTimer() {
    color.toggleBlink(); // 200msec blink

    TIMER_TICK( message_timer );

    TIMER_TICK( reconnect_timer );

    gs.onTimer();

    IR_timer();

    clear_button.onTimer();
}

int8_t onReset() {
    Serial.println(("!E10"));
    Serial.print(P("F: 0x")); Serial.println( freeMemory(), HEX );

    ring_put(&command_queue, COMMAND_SETUP);
    return 0;
}

int8_t onDisconnect() {
    Serial.println(("!E11"));
    Serial.print(P("F: 0x")); Serial.println( freeMemory(), HEX );

    ring_put(&command_queue, COMMAND_CONNECT);
    return 0;
}

void jsonDetectedStart() {
    Serial.println("json<<");

    if (global.buffer_mode != GBufferModeWifiCredentials) {
        IR_state( IR_WRITING );
    }
}

void jsonDetectedData( uint8_t key, uint32_t value ) {
    if ( (IrCtrl.state != IR_WRITING) ||
         (global.buffer_mode != GBufferModeIR) ) {
        return;
    }

    switch (key) {
    case IrJsonParserDataKeyId:
        newest_message_id = value;
        break;
    case IrJsonParserDataKeyFreq:
        IrCtrl.freq = value;
        break;
    case IrJsonParserDataKeyData:
        IR_put( value );
        break;
    default:
        break;
    }
}

void jsonDetectedEnd() {
    Serial.println(">>json");

    if ( (IrCtrl.state != IR_WRITING) ||
         (global.buffer_mode != GBufferModeIR) ) {
        Serial.println("!E5");
        IR_dump();
        return;
    }

    Serial.println(("xmit"));
    IR_xmit();
    color.setLedColor( 0, 0, 1, true, 1 ); // xmit: blue blink for 1sec
}

int8_t onPostMessagesRequest(uint8_t cid, GSwifi::GSREQUESTSTATE state) {
    while (!ring_isempty(gs._buf_cmd)) {
        char letter;
        ring_get(gs._buf_cmd, &letter, 1);

        irjson_parse( letter,
                      &jsonDetectedStart,
                      &jsonDetectedData,
                      &jsonDetectedEnd );
    }

    if (state == GSwifi::GSREQUESTSTATE_RECEIVED) {
        // should be xmitting or idle (xmit finished)
        if (IrCtrl.state == IR_WRITING) {
            Serial.println(("!E7"));
            // invalid json
            gs.writeHead(cid, 400);
            gs.writeEnd();
        }
        else {
            gs.writeHead(cid, 200);
            gs.writeEnd();
        }
        ring_put( &command_queue, COMMAND_CLOSE );
        ring_put( &command_queue, cid );
    }

    return 0;
}

int8_t onPostKeysRequest(uint8_t cid, GSwifi::GSREQUESTSTATE state) {
    if (state == GSwifi::GSREQUESTSTATE_RECEIVED) {
        // don't close other client requests, we can handle multiple concurrent client requests
        // and "close" and it's response mixing up makes things difficult

        // respond to this cid, when we get a new key
        post_keys_cid = cid;

        // delay execution to next tick (we get clean stack)
        // POST /keys to server
        ring_put(&command_queue, COMMAND_POST_KEYS);
    }
}

int8_t onRequest(uint8_t cid, int8_t routeid, GSwifi::GSREQUESTSTATE state) {
    switch (routeid) {
    case 0: // POST /messages
        return onPostMessagesRequest(cid, state);

    case 1: // POST /keys
        // when client requests for a new key,
        // we request server for one, and respond to client with the result from server
        return onPostKeysRequest(cid, state);

    default:
        break;
    }
    return -1;
}

int8_t onPostDoorResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state) {
    Serial.print(P("P /d RS ")); Serial.println(status_code);

    ring_clear(gs._buf_cmd);

    if (state != GSwifi::GSREQUESTSTATE_RECEIVED) {
        return 0;
    }

    switch (status_code) {
    case 200:
        keys.setKeyValid(true);
        // save only independent area, since global.buffer might be populated by IR or so.
        keys.save2();

        ring_put( &command_queue, COMMAND_CLOSE );
        ring_put( &command_queue, cid );
        ring_put( &command_queue, COMMAND_START );

        break;
    case 401:
    case HTTP_STATUSCODE_CLIENT_TIMEOUT:
        // keys have expired, we have to start from morse sequence again
        gs.close(cid);
        keys.clear();
        keys.save();
        break;
    case 408:
    case 503: // heroku responds with 503 if longer than 30sec
    default:
        // try again
        gs.close(cid);
        postDoor();
        break;
    }

    return 0;
}

int8_t onGetMessagesResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state) {
    Serial.print(P("G /m RS ")); Serial.println(status_code);

    if (status_code != 200) {
        ring_clear(gs._buf_cmd);
    }

    switch (status_code) {
    case 200:
        while (!ring_isempty(gs._buf_cmd)) {
            char letter;
            ring_get(gs._buf_cmd, &letter, 1);

            irjson_parse( letter,
                          &jsonDetectedStart,
                          &jsonDetectedData,
                          &jsonDetectedEnd );
        }

        if (state == GSwifi::GSREQUESTSTATE_RECEIVED) {
            // should not be WRITING here, should be XMITTING or IDLE (xmit finished)
            if (IrCtrl.state == IR_WRITING) {
                // prevent from locking in WRITING state forever
                IR_state( IR_IDLE );
            }

            ring_put( &command_queue, COMMAND_CLOSE );
            ring_put( &command_queue, cid );
            ring_put( &command_queue, COMMAND_START );
        }
        break;
    case HTTP_STATUSCODE_CLIENT_TIMEOUT:
        gs.close(cid);
        TIMER_START(message_timer, 5);
        break;
    // heroku responds with 503 if longer than 30sec,
    // or when deploy occurs
    case 503:
    default:
        if (state == GSwifi::GSREQUESTSTATE_RECEIVED) {
            gs.close(cid);
            TIMER_START(message_timer, 5);
        }
        break;
    }

    return 0;
}

int8_t onPostKeysResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state) {
    Serial.print(P("P /k RS ")); Serial.println(status_code);

    if (status_code != 200) {
        ring_clear(gs._buf_cmd);
    }

    if (state != GSwifi::GSREQUESTSTATE_RECEIVED) {
        return 0;
    }

    gs.writeHead( post_keys_cid, status_code );

    switch (status_code) {
    case 200:
        while (!ring_isempty(gs._buf_cmd)) {
            char letter;
            ring_get(gs._buf_cmd, &letter, 1);
            gs.write( letter );
        }
        gs.writeEnd();
        break;
    default:
        gs.writeEnd();
        break;
    }

    ring_put( &command_queue, COMMAND_CLOSE );
    ring_put( &command_queue, cid );
    ring_put( &command_queue, COMMAND_CLOSE );
    if (ring_isfull( &command_queue )) {
        Serial.println(("!E8"));
        return -1;
    }
    ring_put( &command_queue, post_keys_cid );

    return 0;
}

int8_t onPostMessagesResponse(uint8_t cid, uint16_t status_code, GSwifi::GSREQUESTSTATE state) {
    Serial.print(P("P /m RS ")); Serial.println(status_code);

    if (status_code != 200) {
        ring_clear(gs._buf_cmd);
    }

    if (state != GSwifi::GSREQUESTSTATE_RECEIVED) {
        return 0;
    }

    ring_put( &command_queue, COMMAND_CLOSE );
    ring_put( &command_queue, cid );

    return 0;
}

void postDoor() {
    // devicekey=[0-9A-F]{32}&hostname=IRKit%%%%
    char body[POST_DOOR_BODY_LENGTH+1];
    sprintf(body, "devicekey=%s&hostname=%s", keys.getKey(), gs.hostname());
    gs.post( "/d", body, POST_DOOR_BODY_LENGTH, &onPostDoorResponse, 50 );
}

int8_t getMessages() {
    // /m?devicekey=C7363FDA0F06406AB11C29BA41272AE3&newer_than=4294967295
    char path[70];
    sprintf(path, P("/m?devicekey=%s&newer_than=%ld"), keys.getKey(), newest_message_id);
    return gs.get(path, &onGetMessagesResponse, 50);
}

int8_t postMessages() {
    // post body is IR data, move devicekey parameter to query, for implementation simplicity
    // /p?devicekey=C7363FDA0F06406AB11C29BA41272AE3&freq=38
    char path[54];
    sprintf(path, P("/p?devicekey=%s&freq=%d"), keys.getKey(), IrCtrl.freq);
    return gs.postBinary( path,
                          (const char*)global.buffer, IR_packedlength(),
                          &onPostMessagesResponse,
                          10 );
}

int8_t postKeys() {
    // devicekey=[0-9A-F]{32}
    char body[POST_KEYS_BODY_LENGTH+1];
    sprintf(body, "devicekey=%s", keys.getKey());
    return gs.post( "/k",
                    body, POST_KEYS_BODY_LENGTH,
                    &onPostKeysResponse,
                    10 );
}

void connect() {
    global.buffer_mode = GBufferModeWifiCredentials;
    IR_state( IR_DISABLED );
    listener.enable(false);

    // load wifi credentials from EEPROM
    keys.load();

    if (keys.isWifiCredentialsSet()) {
        color.setLedColor( 1, 1, 0, true ); // yellow blink if we have valid keys

        gs.join(keys.getSecurity(),
                keys.getSSID(),
                keys.getPassword());
    }

    if (gs.isJoined()) {
        keys.setWifiWasValid(true);
        keys.save();

        color.setLedColor( 0, 1, 0, true ); // green blink: joined successfully, setting up

        // 0
        gs.registerRoute( GSwifi::GSMETHOD_POST, P("/messages") );
        // 1
        gs.registerRoute( GSwifi::GSMETHOD_POST, P("/keys") );

        gs.setRequestHandler( &onRequest );

        // start http server
        gs.listen(80);

        // start mDNS
        gs.setupMDNS();
    }
    else {
        Serial.println(("!E9"));
        keys.dump();

        if (keys.wasWifiValid()) {
            // retry
            color.setLedColor( 1, 0, 0, false ); // red: error
            TIMER_START(reconnect_timer, 5);
        }
        else {
            keys.clear();
            color.setLedColor( 1, 0, 0, true ); // red blink: listening for morse
            listener.enable(true);
        }
    }

    if (gs.isListening()) {
        color.setLedColor( 0, 0, 1, false ); // blue: ready

        if (keys.isAPIKeySet() && ! keys.isValid()) {
            postDoor();
        }
        else if (keys.isValid()) {
            ring_put( &command_queue, COMMAND_START );
        }
    }
}

void startNormalOperation() {
    TIMER_START(message_timer, 0);

    global.buffer_mode = GBufferModeIR;

    IR_state( IR_IDLE );
}

void letterCallback( char letter ) {
    Serial.print("L:"); Serial.write(letter); Serial.println();

    if (morse_error) {
        return;
    }

    int8_t result = keys.put( letter );
    if (result != 0) {
        // postpone til this "word" ends
        morse_error = true;
    }
}

void wordCallback() {
    Serial.println("W");

    if (morse_error) {
        morse_error = false;
        keys.clear();
        return;
    }

    int8_t result = keys.putDone();
    if ( result != 0 ) {
        keys.clear();
    }
    else {
        keys.dump();
        keys.save();
        connect();
    }
}

void IRKit_setup() {
    ring_init( &command_queue, command_queue_data, COMMAND_QUEUE_SIZE + 1 );

    //--- initialize LED

    FlexiTimer2::set( TIMER_INTERVAL, &onTimer );
    FlexiTimer2::start();
    color.setLedColor( 1, 0, 0, false ); // red: error

    //--- initialize long press button

    clear_button.callback = &longPressed;

    //--- initialize morse listener

    pinMode(MICROPHONE,  INPUT);

    listener.letterCallback = &letterCallback;
    listener.wordCallback   = &wordCallback;
    listener.setup();

    //--- initialize IR

    pinMode(IR_OUT,           OUTPUT);

    // pull-up
    pinMode(IR_IN,            INPUT);
    digitalWrite(IR_IN,       HIGH);

    IR_initialize( &onIRReceive );

    //--- initialize Wifi

    pinMode(LDO33_ENABLE,     OUTPUT);
    reset3V3();

    gs.setup( &onDisconnect, &onReset );

    connect();
}

void IRKit_loop() {
    global.loop(); // always run first

    listener.loop();

    timerLoop();

    clear_button.loop();

    // wifi
    gs.loop();

    IR_loop();

    // Wifi UART interface test
    if (Serial.available()) {
        static uint8_t last_character = '0';
        static bool command_mode = false;
        last_character = Serial.read();

        Serial.write(last_character);
        Serial.println();
        Serial.print(P("F: 0x")); Serial.println( freeMemory(), HEX );

        if (last_character == 0x1B) {
            command_mode = ! command_mode;
            Serial.print("command_mode:"); Serial.println(command_mode);
        }

        if (command_mode) {
            Serial1X.write(last_character);
        }
        else if (last_character == 'd') {
            keys.load();

            Serial.println();
            keys.dump();

            // Serial.println();
            // gs.dump();

            Serial.println();
            IR_dump();
            Serial.println();
        }
        else if (last_character == 'l') {
            longPressed();
        }
        // else if (last_character == 's') {
        //     keys.set(GSSECURITY_WPA2_PSK,
        //              PB("Rhodos",1),
        //              PB("aaaaaaaaaaaaa",2));
        //     keys.setKey(P("5284CF0D43994784897ECAB3D9935498"));
        //     keys.save();
        // }
    }
}
