#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "credentials.h" // Ensure this file contains your WiFi and MQTT credentials

// ======= Constants =======
const char* SOFTWARE_VERSION = "1.0.0";
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;
const int LINE_HEIGHT = 30;
const int MENU_TOP_OFFSET = 40;
const int STATUS_BAR_HEIGHT = 40;

// ======= Timeout Parameters =======
const unsigned long SCREEN_TIMEOUT = 30000; // 30 seconds
unsigned long last_activity_time = 0;       // Timestamp of the last user interaction
bool screen_asleep = false;                 // Screen state

// ======= MQTT Topics =======
const char* fridge_status_topic = "home/m5stack/core2/fridge_door/status";
const char* freezer_status_topic = "home/m5stack/core2/freezer_door/status";
const char* scenes_control_topic = "home/m5stack/core2/scenes/control";

// ======= Devices and Scenes =======
struct Device {
    const char* name;
    const char* control_topic;
    bool is_active;
};

Device devices[] = {
    {"Hallway Lights", "home/m5stack/core2/devices/hallway/control", false},
    {"Living Room Tree", "home/m5stack/core2/devices/living_tree/control", false},
    {"Left Lamp", "home/m5stack/core2/devices/left_lamp/control", false},
    {"Right Lamp 1", "home/m5stack/core2/devices/right_lamp1/control", false},
    {"Right Lamp 2", "home/m5stack/core2/devices/right_lamp2/control", false},
    {"Spotlight", "home/m5stack/core2/devices/spotlight/control", false}
};
const int num_devices = sizeof(devices) / sizeof(devices[0]);

const char* scenes[] = {
    "Bright/Normal", "Christmas", "Freezer/Fridge", "Seahawks", "Sounders",
    "Vibes", "Warm", "Warm Bright", "Custom Scene 1", "Custom Scene 2"
};
const int num_scenes = sizeof(scenes) / sizeof(scenes[0]);

// ======= Menu Items Defined Separately =======
const char* main_menu_items[] = {"Devices", "Scenes", "Power Off All Devices", "Exit"};
const char* devices_menu_items[] = {"Hallway Lights", "Living Room Tree", "Left Lamp", "Right Lamp 1", "Right Lamp 2", "Spotlight", "< Back>"};
const char* scenes_menu_items[] = {"Bright/Normal", "Christmas", "Freezer/Fridge", "Seahawks", "Sounders", "Vibes", "Warm", "Warm Bright", "Custom Scene 1", "Custom Scene 2", "< Back>"};

// ======= Global Variables =======
enum MenuState { MAIN_MENU, DEVICES_MENU, SCENES_MENU };
MenuState current_menu = MAIN_MENU;

int selected_index = 0; // Currently selected item in the menu
int scroll_offset = 0;  // Tracks the first visible item in the menu
const int max_visible_items = (SCREEN_HEIGHT - MENU_TOP_OFFSET - STATUS_BAR_HEIGHT) / LINE_HEIGHT;

bool fridge_open = false;  // Fridge door status
bool freezer_open = false; // Freezer door status

bool alert_active = false; // Alert state
String alert_message = "";  // Alert message to display

WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// ======= Function Prototypes =======
void setup_wifi();
void reconnect_mqtt();
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void draw_menu(const char* title, const char* items[], int num_items);
void navigate_menu(int direction);
void select_menu_item();
void toggle_device(int index);
void apply_scene(int index);
void power_off_all_devices();
void draw_status_bar();
void update_fridge_freezer_status(const char* topic, bool is_open);
void handle_alert();
void clear_alert();
void handle_screen_timeout();
void wakeup_screen();
void sleep_screen();

// ======= Setup =======
void setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);

    M5.Lcd.setRotation(1);
    M5.Lcd.fillScreen(TFT_BLACK);

    // Initialize GPIO pins for devices if needed
    // Example: pinMode(GPIO_PIN, OUTPUT);

    setup_wifi();  // Connect to Wi-Fi

    mqtt_client.setServer(MQTT_SERVER, MQTT_PORT);
    mqtt_client.setCallback(mqtt_callback);  // Set the callback function for MQTT messages

    last_activity_time = millis(); // Initialize the last activity timestamp

    // Connect to MQTT Broker
    reconnect_mqtt();

    // Draw the Main Menu
    draw_menu("Main Menu", main_menu_items, 4);
}

// ======= Main Loop =======
void loop() {
    // Handle MQTT connection
    if (!mqtt_client.connected()) {
        reconnect_mqtt();
    }
    mqtt_client.loop();

    // Handle M5Stack Core2 tasks
    M5.update();

    // Detect any user interactions
    bool any_button_pressed = false;
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
        any_button_pressed = true;
        last_activity_time = millis(); // Reset the timeout timer
    }

    // If any button was pressed, handle wakeup and alert acknowledgment
    if (any_button_pressed) {
        if (screen_asleep) {
            wakeup_screen();
        }
        if (alert_active) {
            clear_alert();
        }
    }

    // Handle specific button presses for navigation and selection
    if (M5.BtnA.wasPressed()) navigate_menu(-1); // Move up
    if (M5.BtnC.wasPressed()) navigate_menu(1);  // Move down
    if (M5.BtnB.wasPressed()) select_menu_item(); // Select item

    // Handle screen timeout
    handle_screen_timeout();
}

// ======= WiFi Setup =======
void setup_wifi() {
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println("Connecting to WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Wait until connected
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        M5.Lcd.print(".");
    }

    M5.Lcd.println("\nConnected!");
    M5.Lcd.printf("IP: %s\n", WiFi.localIP().toString().c_str());
}

// ======= MQTT Reconnect =======
void reconnect_mqtt() {
    // Loop until reconnected
    while (!mqtt_client.connected()) {
        M5.Lcd.print("Attempting MQTT connection...");
        // Attempt to connect
        if (MQTT_USER[0] != '\0') {
            // If username is set
            if (mqtt_client.connect("M5StackCore2", MQTT_USER, MQTT_PASSWORD)) {
                M5.Lcd.println("connected");
                // Subscribe to fridge and freezer status topics
                mqtt_client.subscribe(fridge_status_topic);
                mqtt_client.subscribe(freezer_status_topic);
            } else {
                M5.Lcd.print("failed, rc=");
                M5.Lcd.print(mqtt_client.state());
                M5.Lcd.println(" try again in 5 seconds");
                delay(5000);
            }
        } else {
            // If no username
            if (mqtt_client.connect("M5StackCore2")) {
                M5.Lcd.println("connected");
                // Subscribe to fridge and freezer status topics
                mqtt_client.subscribe(fridge_status_topic);
                mqtt_client.subscribe(freezer_status_topic);
            } else {
                M5.Lcd.print("failed, rc=");
                M5.Lcd.print(mqtt_client.state());
                M5.Lcd.println(" try again in 5 seconds");
                delay(5000);
            }
        }
    }
}

// ======= MQTT Callback =======
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    // Safely convert payload to String without modifying the original buffer
    String msg;
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }
    msg.trim(); // Remove any leading/trailing whitespace

    // Debugging: Print received MQTT message
    Serial.print("Received message on topic: ");
    Serial.print(topic);
    Serial.print(" with payload: ");
    Serial.println(msg);

    // Handle Fridge status update
    if (strcmp(topic, fridge_status_topic) == 0) {
        update_fridge_freezer_status(topic, msg.equalsIgnoreCase("OPEN"));
    }

    // Handle Freezer status update
    if (strcmp(topic, freezer_status_topic) == 0) {
        update_fridge_freezer_status(topic, msg.equalsIgnoreCase("OPEN"));
    }
}

// ======= Update Fridge/Freezer Status =======
void update_fridge_freezer_status(const char* topic, bool is_open) {
    if (strcmp(topic, fridge_status_topic) == 0) {
        fridge_open = is_open;
        if (is_open) {
            alert_active = true;
            alert_message = "Fridge Door Open!";
        } else {
            alert_active = false;
            alert_message = "";
        }
    }
    if (strcmp(topic, freezer_status_topic) == 0) {
        freezer_open = is_open;
        if (is_open) {
            alert_active = true;
            alert_message = "Freezer Door Open!";
        } else {
            alert_active = false;
            alert_message = "";
        }
    }
    draw_status_bar();
    draw_menu(
        (current_menu == MAIN_MENU) ? "Main Menu" :
        (current_menu == DEVICES_MENU) ? "Devices" :
        "Scenes",
        (current_menu == MAIN_MENU) ? main_menu_items :
        (current_menu == DEVICES_MENU) ? devices_menu_items :
        scenes_menu_items,
        (current_menu == MAIN_MENU) ? 4 :
        (current_menu == DEVICES_MENU) ? (num_devices + 1) :
        (num_scenes + 1)
    );
}

// ======= Handle Alert =======
void handle_alert() {
    if (alert_active) {
        // Display alert message
        M5.Lcd.setTextSize(3);
        M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
        M5.Lcd.setCursor(10, SCREEN_HEIGHT / 2 - 20);
        M5.Lcd.printf("%s", alert_message.c_str());

        // Optionally, add more visual indicators like flashing text or changing background color
    }
}

// ======= Clear Alert =======
void clear_alert() {
    alert_active = false;
    alert_message = "";

    // Clear the alert message from the display
    // Redraw the current menu
    draw_menu(
        (current_menu == MAIN_MENU) ? "Main Menu" :
        (current_menu == DEVICES_MENU) ? "Devices" :
        "Scenes",
        (current_menu == MAIN_MENU) ? main_menu_items :
        (current_menu == DEVICES_MENU) ? devices_menu_items :
        scenes_menu_items,
        (current_menu == MAIN_MENU) ? 4 :
        (current_menu == DEVICES_MENU) ? (num_devices + 1) :
        (num_scenes + 1)
    );
}

// ======= Handle Screen Timeout =======
void handle_screen_timeout() {
    unsigned long current_time = millis();
    if (!alert_active && (current_time - last_activity_time > SCREEN_TIMEOUT) && !screen_asleep) {
        sleep_screen();
    } else if ((current_time - last_activity_time <= SCREEN_TIMEOUT) && screen_asleep) {
        wakeup_screen();
    }
}

// ======= Sleep Screen =======
void sleep_screen() {
    M5.Lcd.sleep();                       // Put the LCD to sleep
    M5.Lcd.fillScreen(TFT_BLACK);        // Ensure the screen is blacked out
    screen_asleep = true;
    Serial.println("Screen asleep due to inactivity.");
}

// ======= Wake Up Screen =======
void wakeup_screen() {
    M5.Lcd.wakeup();                      // Wake the LCD up
    M5.Lcd.fillScreen(TFT_BLACK);        // Redraw the screen if necessary
    draw_menu(
        (current_menu == MAIN_MENU) ? "Main Menu" :
        (current_menu == DEVICES_MENU) ? "Devices" :
        "Scenes",
        (current_menu == MAIN_MENU) ? main_menu_items :
        (current_menu == DEVICES_MENU) ? devices_menu_items :
        scenes_menu_items,
        (current_menu == MAIN_MENU) ? 4 :
        (current_menu == DEVICES_MENU) ? (num_devices + 1) :
        (num_scenes + 1)
    );
    screen_asleep = false;
    Serial.println("Screen woke up due to user interaction.");
}

// ======= Menu Drawing =======
void draw_menu(const char* title, const char* items[], int num_items) {
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setCursor(10, 10);
    M5.Lcd.println(title);

    int start_index = scroll_offset;
    int end_index = min(scroll_offset + max_visible_items, num_items);

    for (int i = start_index; i < end_index; i++) {
        int y = MENU_TOP_OFFSET + (i - scroll_offset) * LINE_HEIGHT;
        M5.Lcd.setCursor(20, y);
        if (i == selected_index) {
            M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
            M5.Lcd.printf("> %s", items[i]);
        } else {
            M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
            M5.Lcd.printf("  %s", items[i]);
        }
    }

    // Handle Alerts
    if (alert_active) {
        handle_alert();
    } else {
        draw_status_bar();
    }
}

// ======= Status Bar =======
void draw_status_bar() {
    // Clear the status bar area
    M5.Lcd.fillRect(0, SCREEN_HEIGHT - STATUS_BAR_HEIGHT, SCREEN_WIDTH, STATUS_BAR_HEIGHT, TFT_DARKGRAY);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_DARKGRAY);
    M5.Lcd.setCursor(10, SCREEN_HEIGHT - STATUS_BAR_HEIGHT + 10);
    M5.Lcd.printf("FRZR: %s  FRDG: %s", freezer_open ? "OPEN" : "CLOSED", fridge_open ? "OPEN" : "CLOSED");
}

// ======= Navigation =======
void navigate_menu(int direction) {
    int num_items = (current_menu == MAIN_MENU) ? 4 :
                    (current_menu == DEVICES_MENU) ? (num_devices + 1) :
                    (num_scenes + 1);

    selected_index = (selected_index + direction + num_items) % num_items;

    // Adjust scroll offset
    if (selected_index < scroll_offset) {
        scroll_offset = selected_index;
    } else if (selected_index >= scroll_offset + max_visible_items) {
        scroll_offset = selected_index - max_visible_items + 1;
    }

    if (current_menu == MAIN_MENU) draw_menu("Main Menu", main_menu_items, 4);
    else if (current_menu == DEVICES_MENU) draw_menu("Devices", devices_menu_items, num_devices + 1);
    else if (current_menu == SCENES_MENU) draw_menu("Scenes", scenes_menu_items, num_scenes + 1);
}

// ======= Menu Selection =======
void select_menu_item() {
    if (current_menu == MAIN_MENU) {
        if (selected_index == 0) {
            current_menu = DEVICES_MENU;
        }
        else if (selected_index == 1) {
            current_menu = SCENES_MENU;
        }
        else if (selected_index == 2) {
            power_off_all_devices();
        }
        else {
            M5.Lcd.fillScreen(TFT_BLACK); // Exit
            // Optionally, implement an exit function or power off
            // For example, enter deep sleep or reset
            // ESP.restart(); // Uncomment to restart the device
        }
    } else if (current_menu == DEVICES_MENU) {
        if (selected_index == num_devices) {
            current_menu = MAIN_MENU; // Back
        }
        else {
            toggle_device(selected_index);
        }
    } else if (current_menu == SCENES_MENU) {
        if (selected_index == num_scenes) {
            current_menu = MAIN_MENU; // Back
        }
        else {
            apply_scene(selected_index);
        }
    }
    selected_index = 0;  // Reset selection on menu change
    scroll_offset = 0;   // Reset scroll offset on menu change
    navigate_menu(0);    // Redraw menu
}

// ======= Power Off All Devices =======
void power_off_all_devices() {
    // Loop through each device and send OFF message
    for (int i = 0; i < num_devices; i++) {
        devices[i].is_active = false;
        mqtt_client.publish(devices[i].control_topic, "OFF");

        // Debugging: Print device power off message
        Serial.print("Turning off device: ");
        Serial.println(devices[i].name);
    }
    // Optionally, provide user feedback
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setCursor(10, SCREEN_HEIGHT / 2 - 10);
    M5.Lcd.println("All Devices Off");
    delay(2000); // Display message for 2 seconds
    navigate_menu(0); // Return to main menu
}

// ======= Toggle Device =======
void toggle_device(int index) {
    if (index >= 0 && index < num_devices) {
        devices[index].is_active = !devices[index].is_active;
        mqtt_client.publish(devices[index].control_topic, devices[index].is_active ? "ON" : "OFF");

        // Debugging: Print device state and published message
        Serial.print("Toggling device: ");
        Serial.print(devices[index].name);
        Serial.print(" State: ");
        Serial.println(devices[index].is_active ? "ON" : "OFF");

        // Optionally, provide user feedback
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Lcd.setCursor(10, SCREEN_HEIGHT / 2 - 10);
        M5.Lcd.printf("%s %s", devices[index].name, devices[index].is_active ? "ON" : "OFF");
        delay(1000); // Display message for 1 second
        navigate_menu(0); // Return to previous menu
    }
}

// ======= Apply Scene =======
void apply_scene(int index) {
    if (index >= 0 && index < num_scenes) {
        mqtt_client.publish(scenes_control_topic, scenes[index]);

        // Debugging: Print applied scene
        Serial.print("Applying scene: ");
        Serial.println(scenes[index]);

        // Optionally, provide user feedback
        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setTextSize(2);
        M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Lcd.setCursor(10, SCREEN_HEIGHT / 2 - 10);
        M5.Lcd.printf("Scene: %s", scenes[index]);
        delay(2000); // Display message for 2 seconds
        navigate_menu(0); // Return to previous menu
    }
}
