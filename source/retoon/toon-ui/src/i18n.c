/* The screen in Dutch (the default) or English: CONFIG_DIR/language holds
 * "lang=en" or "lang=nl". The texts in the code are English; tr() gives the
 * Dutch one from the table below, or the English text when the language is
 * English or the text is not in the table. A changed language takes effect
 * when toon-ui starts again (the web interface restarts it). */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "ui.h"

bool lang_nl = true;

static const char *const nl[][2] = {
	/* states and cards */
	{ "Unavailable", "Niet beschikbaar" },
	{ "Unknown", "Onbekend" },
	{ "Scene", "Scène" },
	{ "Script", "Script" },
	{ "Press", "Indrukken" },
	{ "Open", "Open" },
	{ "Closed", "Dicht" },
	{ "Detected", "Gedetecteerd" },
	{ "Clear", "Vrij" },
	{ "Connected", "Verbonden" },
	{ "Disconnected", "Niet verbonden" },
	{ "Problem", "Probleem" },
	{ "Heating", "Verwarmt" },
	{ "Heating · %.1f °C", "Verwarmt · %.1f °C" },
	{ "Off", "Uit" },
	{ "Page %d", "Pagina %d" },
	{ "Cooling", "Koelt" },
	{ "Mode: cooling", "Modus: koelen" },
	{ "Mode: heating", "Modus: verwarming" },
	{ "Idle", "Rust" },
	{ "No boiler module", "Geen ketelmodule" },
	{ "External device", "Extern apparaat" },
	{ "Solar", "Zon" },
	{ "From the grid", "Van het net" },
	{ "To the grid", "Naar het net" },
	{ "House", "Huis" },
	{ "Grid", "Net" },
	{ "Battery", "Batterij" },
	{ "Energy usage", "Energieverbruik" },
	{ "Solar production", "Zonne-opbrengst" },
	{ "Gas usage", "Gasverbruik" },
	{ "Waiting for Home Assistant's energy settings…", "Wachten op de energie-instellingen van Home Assistant…" },
	{ "No energy settings in Home Assistant", "Geen energie-instellingen in Home Assistant" },
	{ "Energy cards need a link to Home Assistant", "Energiekaarten hebben een koppeling met Home Assistant nodig" },
	{ "Home Assistant's energy settings have nothing for this card",
	  "De energie-instellingen van Home Assistant hebben niets voor deze kaart" },
	{ "Room", "Kamer" },
	{ "Set to", "Ingesteld" },
	{ "Target", "Doel" },
	{ "%s card: not shown on the ReToon", "Kaart %s: niet te tonen op de ReToon" },
	{ "This card: not shown on the ReToon", "Deze kaart: niet te tonen op de ReToon" },
	{ "Waiting for Home Assistant", "Wacht op Home Assistant" },
	{ "Loading…", "Laden…" },
	{ "No history yet", "Nog geen geschiedenis" },
	{ "App \"%s\" is not installed", "App \"%s\" is niet geïnstalleerd" },
	{ "needs a newer ReToon. Please update it (System in the web interface).",
	  "heeft een nieuwere ReToon nodig. Werk hem bij (Systeem in de webinterface)." },
	/* the ReToon's own values */
	{ "Good", "Goed" },
	{ "Fair", "Redelijk" },
	{ "Moderate", "Matig" },
	{ "Poor", "Slecht" },
	{ "Measuring…", "Aan het meten…" },
	{ "%s · measuring…", "%s · meten…" },
	{ "Getting worse", "Wordt slechter" },
	{ "Getting better", "Wordt beter" },
	{ "%s · learning", "%s · lerend" },
	{ "Room temperature", "Kamertemperatuur" },
	{ "Internal temperature", "Temperatuur in de Toon" },
	{ "Outside temperature", "Buitentemperatuur" },
	{ "Humidity", "Luchtvochtigheid" },
	{ "Light", "Licht" },
	{ "Air quality", "Luchtkwaliteit" },
	{ "Flow temperature", "Aanvoertemperatuur" },
	{ "Gas", "Gas" },
	{ "Water", "Water" },
	{ "Gas flow", "Gasverbruik nu" },
	{ "Water flow", "Waterverbruik nu" },
	{ "Heat", "Warmte" },
	{ "Electricity import (meter sensor)", "Stroom afgenomen (metersensor)" },
	{ "Electricity export (meter sensor)", "Stroom teruggeleverd (metersensor)" },
	{ "Electricity import tariff 1", "Stroom afgenomen tarief 1" },
	{ "Electricity import tariff 2", "Stroom afgenomen tarief 2" },
	{ "Electricity export tariff 1", "Stroom teruggeleverd tarief 1" },
	{ "Electricity export tariff 2", "Stroom teruggeleverd tarief 2" },
	{ "%s, power", "%s, vermogen" },
	{ "Return water", "Retourwater" },
	{ "Flow setpoint", "Gevraagde aanvoertemperatuur" },
	{ "Hot water setpoint", "Gevraagde warmwatertemperatuur" },
	{ "Hot water", "Warm water" },
	{ "Active", "Actief" },
	{ "Maximum heating setpoint", "Maximale aanvoertemperatuur" },
	{ "Modulation", "Modulatie" },
	{ "Water pressure", "Waterdruk" },
	{ "OpenTherm module temperature", "Temperatuur OpenTherm-module" },
	{ "Fault code", "Foutcode" },
	{ "Connection status", "Status aansluiting" },
	{ "Burner", "Brander" },
	{ "Locked", "Vastgezet" },
	{ "Hot water flow", "Warmwaterdoorstroming" },
	{ "Solar storage temperature", "Temperatuur zonneboiler" },
	{ "Solar collector temperature", "Temperatuur zonnecollector" },
	{ "Flow temperature circuit 2", "Aanvoertemperatuur circuit 2" },
	{ "Hot water temperature 2", "Warmwatertemperatuur 2" },
	{ "Exhaust temperature", "Rookgastemperatuur" },
	{ "Heat exchanger temperature", "Temperatuur warmtewisselaar" },
	{ "Flame current", "Vlamstroom" },
	{ "Heating active", "Verwarming actief" },
	{ "Hot water active", "Warm water actief" },
	{ "Cooling active", "Koelen actief" },
	{ "Remote room setpoint", "Kamertemperatuur op afstand" },
	{ "Maximum power", "Maximaal vermogen" },
	{ "Minimum modulation", "Minimale modulatie" },
	{ "Hot water temperature", "Warmwatertemperatuur" },
	{ "Fan speed setpoint", "Ingestelde ventilatorsnelheid" },
	{ "Fan speed", "Ventilatorsnelheid" },
	{ "Hot water setpoint upper limit", "Bovengrens warmwatertemperatuur" },
	{ "Hot water setpoint lower limit", "Ondergrens warmwatertemperatuur" },
	{ "Unsuccessful burner starts", "Mislukte branderstarts" },
	{ "Flame signal too low", "Vlamsignaal te laag" },
	{ "OEM diagnostic code", "Diagnosecode fabrikant" },
	{ "Burner starts", "Branderstarts" },
	{ "Heating pump starts", "Starts pomp verwarming" },
	{ "Hot water pump starts", "Starts pomp warm water" },
	{ "Hot water burner starts", "Branderstarts warm water" },
	{ "Burner hours", "Branderuren" },
	{ "Heating pump hours", "Draaiuren pomp verwarming" },
	{ "Hot water pump hours", "Draaiuren pomp warm water" },
	{ "Hot water burner hours", "Branderuren warm water" },
	{ "OpenTherm version (installation)", "OpenTherm-versie (installatie)" },
	{ "Installation product type", "Producttype installatie" },
	{ "Installation product version", "Productversie installatie" },
	{ "Heat request", "Warmtevraag" },
	{ "Cool request", "Koelvraag" },
	{ "Installation fault", "Storing installatie" },
	{ "Thermostat", "Thermostaat" },
	{ "Connection", "Aansluiting" },
	{ "Display", "Scherm" },
	{ "WiFi signal", "WiFi-signaal" },
	{ "Frost protection", "Vorstbeveiliging" },
	{ "Warming up for %s at %s", "Opwarmen voor %s om %s" },
	{ "Until %s", "Tot %s" },
	{ "Temporary until %s", "Tijdelijk tot %s" },
	{ "Then %s at %s", "Daarna %s om %s" },
	/* information and restart */
	{ "Restarting...", "Herstarten…" },
	{ "Restart", "Herstarten" },
	{ "Cancel", "Annuleren" },
	{ "Yes", "Ja" },
	{ "Are you sure?", "Weet je het zeker?" },
	{ "Close", "Sluiten" },
	{ "No picture from this camera", "Geen beeld van deze camera" },
	{ "Restart the Toon now? ReToon starts again.", "De Toon nu herstarten? ReToon start daarna weer." },
	{ "Restart the Toon now? It will start the original Toon software, because "
	  "\"Always start ReToon\" is off (web interface, System).",
	  "De Toon nu herstarten? Dan start de originele Toon-software, want "
	  "\"Altijd ReToon starten\" staat uit (webinterface, Systeem)." },
	{ "This ReToon", "Deze ReToon" },
	{ "version %s", "versie %s" },
	{ "own build", "eigen build" },
	{ "Pages, the Home Assistant connection and all other settings are "
	  "made in a browser on your phone or computer. Open the address below.",
	  "Pagina's, de koppeling met Home Assistant en alle andere instellingen "
	  "regel je in een browser op je telefoon of computer. Open het adres hieronder." },
	{ "Web interface", "Webinterface" },
	{ "or", "of" },
	{ "User name", "Gebruikersnaam" },
	{ "Password", "Wachtwoord" },
	{ "Name", "Naam" },
	{ "Scan to open it, logged in", "Scan om hem ingelogd te openen" },
	{ "The browser warns once that it does not know this ReToon's "
	  "certificate. Its SHA-256 fingerprint: %s",
	  "De browser waarschuwt één keer dat hij het certificaat van deze ReToon "
	  "niet kent. De SHA-256-vingerafdruk: %s" },
	/* connection states (ha.c, mqtt.c keep them in English for the web) */
	{ "Not configured", "Niet ingesteld" },
	{ "Connecting", "Verbinden" },
	{ "Connection lost", "Verbinding verbroken" },
	{ "Home Assistant refused the login", "Home Assistant weigerde de login" },
	{ "Bad Home Assistant address", "Ongeldig adres van Home Assistant" },
	{ "Home Assistant is not reachable", "Home Assistant is niet bereikbaar" },
	{ "The certificate of Home Assistant is not trusted", "Het certificaat van Home Assistant wordt niet vertrouwd" },
	{ "No secure connection to Home Assistant", "Geen beveiligde verbinding met Home Assistant" },
	{ "That address is not Home Assistant", "Dat adres is geen Home Assistant" },
	{ "Broker not reachable", "Broker niet bereikbaar" },
	/* touch calibration (touchcal.c) */
	{ "Touch calibration", "Aanraakscherm kalibreren" },
	{ "Tap the centre of the cross, as precisely as you can (%d of 4).",
	  "Tik zo precies mogelijk op het midden van het kruis (%d van 4)." },
	{ "That did not work out. Tap each cross once, in its centre. Again (1 of 4).",
	  "Dat lukte niet. Tik één keer op het midden van elk kruis. Opnieuw (1 van 4)." },
	{ "Tap Keep to use the new calibration. Without a tap the old one returns.",
	  "Tik op Bewaren om de nieuwe kalibratie te gebruiken. Zonder tik komt de oude terug." },
	{ "Tap Keep to use the new calibration. Without a tap the old one returns in %d s.",
	  "Tik op Bewaren om de nieuwe kalibratie te gebruiken. Zonder tik komt de oude over %d s terug." },
	{ "Keep", "Bewaren" },
	{ "Again", "Opnieuw" },
	/* WiFi */
	{ "Connected to %s  ·  %d dBm  ·  %s", "Verbonden met %s  ·  %d dBm  ·  %s" },
	{ "the network", "het netwerk" },
	{ "WiFi is not set up yet. Choose your network.", "WiFi is nog niet ingesteld. Kies je netwerk." },
	{ "Not connected", "Niet verbonden" },
	{ "Could not connect", "Verbinden lukte niet" },
	{ "The password must be 8 to 63 characters.", "Het wachtwoord moet 8 tot 63 tekens lang zijn." },
	{ "Check the password. The previous settings are in use again.",
	  "Controleer het wachtwoord. De vorige instellingen worden weer gebruikt." },
	{ "Back", "Terug" },
	{ "Connecting to %s…", "Verbinden met %s…" },
	{ "Password for %s", "Wachtwoord voor %s" },
	{ "Name of the network", "Naam van het netwerk" },
	{ "Looking for networks…", "Netwerken zoeken…" },
	{ "No networks found.", "Geen netwerken gevonden." },
	{ "Other network…", "Ander netwerk…" },
	{ "Searching…", "Zoeken…" },
	{ "Search again", "Opnieuw zoeken" },
	/* rescue mode */
	{ "ReToon rescue mode", "ReToon-noodmodus" },
	{ "ReToon could not start: %s.", "ReToon kon niet starten: %s." },
	{ "unknown reason", "onbekende reden" },
	{ "This is a small rescue system. Nothing is lost: your settings and the "
	  "original Toon software are still there. From a computer on the same "
	  "network the system can be repaired or put back.",
	  "Dit is een klein noodsysteem. Er is niets verloren: je instellingen en de "
	  "originele Toon-software zijn er nog. Vanaf een computer in hetzelfde "
	  "netwerk kan het systeem worden hersteld of teruggezet." },
	{ "Try ReToon again", "ReToon opnieuw proberen" },
	{ "Start the original Toon software", "Originele Toon-software starten" },
	{ "Both buttons on the right restart the Toon. If the cause is not solved, "
	  "\"Try ReToon again\" comes back to this screen.",
	  "Beide knoppen rechts herstarten de Toon. Is de oorzaak niet opgelost, dan "
	  "komt \"ReToon opnieuw proberen\" terug op dit scherm." },
	{ "On the network as %s", "In het netwerk als %s" },
	{ "No WiFi settings: press WiFi and choose your network", "Geen WiFi-instellingen: druk op WiFi en kies je netwerk" },
	{ "Not on the network (yet). Press WiFi to choose a network", "(Nog) niet in het netwerk. Druk op WiFi om een netwerk te kiezen" },
	{ "Log in with:  ssh root@%s   (password: toon)", "Inloggen met:  ssh root@%s   (wachtwoord: toon)" },
};

/* Home Assistant's state words, presets and modes, as Home Assistant's own
   Dutch translation shows them */
static const char *const states_nl[][2] = {
	{ "on", "Aan" }, { "off", "Uit" },
	{ "open", "Open" }, { "closed", "Dicht" }, { "opening", "Gaat open" }, { "closing", "Gaat dicht" },
	{ "home", "Thuis" }, { "not_home", "Afwezig" }, { "away", "Weg" },
	{ "comfort", "Comfort" }, { "sleep", "Slapen" }, { "eco", "Eco" }, { "boost", "Boost" },
	{ "activity", "Activiteit" }, { "none", "Geen" }, { "manual", "Handmatig" },
	{ "heat", "Verwarmen" }, { "cool", "Koelen" }, { "auto", "Automatisch" },
	{ "heat_cool", "Verwarmen/koelen" }, { "dry", "Drogen" }, { "fan_only", "Alleen ventilator" },
	{ "low", "Laag" }, { "medium", "Midden" }, { "middle", "Midden" }, { "high", "Hoog" },
	{ "quiet", "Stil" }, { "diffuse", "Diffuus" },
	{ "vertical", "Verticaal" }, { "horizontal", "Horizontaal" }, { "both", "Beide" },
	{ "heating", "Verwarmt" }, { "cooling", "Koelt" }, { "idle", "Inactief" },
	{ "playing", "Speelt af" }, { "paused", "Gepauzeerd" }, { "standby", "Stand-by" },
	{ "buffering", "Bufferen" },
	{ "locked", "Vergrendeld" }, { "unlocked", "Ontgrendeld" }, { "locking", "Vergrendelen" },
	{ "unlocking", "Ontgrendelen" }, { "jammed", "Vastgelopen" },
	{ "armed_away", "Ingeschakeld (weg)" }, { "armed_home", "Ingeschakeld (thuis)" },
	{ "armed_night", "Ingeschakeld (nacht)" }, { "armed_vacation", "Ingeschakeld (vakantie)" },
	{ "disarmed", "Uitgeschakeld" }, { "triggered", "Alarm" }, { "pending", "In afwachting" },
	{ "arming", "Inschakelen" },
	{ "cleaning", "Schoonmaken" }, { "docked", "Gedockt" }, { "returning", "Keert terug" },
	{ "charging", "Opladen" }, { "discharging", "Ontladen" },
	{ "above_horizon", "Boven de horizon" }, { "below_horizon", "Onder de horizon" },
	{ "clear-night", "Helder" }, { "cloudy", "Bewolkt" }, { "fog", "Mist" }, { "hail", "Hagel" },
	{ "lightning", "Onweer" }, { "lightning-rainy", "Onweer en regen" },
	{ "partlycloudy", "Half bewolkt" }, { "pouring", "Stortregen" }, { "rainy", "Regen" },
	{ "snowy", "Sneeuw" }, { "snowy-rainy", "Natte sneeuw" }, { "sunny", "Zonnig" },
	{ "windy", "Winderig" }, { "windy-variant", "Winderig" }, { "exceptional", "Uitzonderlijk" },
	{ "good", "Goed" }, { "fair", "Redelijk" }, { "moderate", "Matig" }, { "poor", "Slecht" },
	{ "OpenTherm", "OpenTherm" }, { "On/off", "Aan/uit" }, { "Detecting", "Zoeken" },
};

void lang_load(void)
{
	char buf[8];

	lang_nl = file_value(LANG_CONF, "lang", buf, sizeof(buf)) || strcmp(buf, "en");
}

const char *tr(const char *en)
{
	if (!lang_nl || !en)
		return en;
	for (size_t i = 0; i < sizeof(nl) / sizeof(nl[0]); i++)
		if (!strcmp(nl[i][0], en))
			return nl[i][1];
	return en;
}

const char *state_text(const char *state)
{
	if (!lang_nl || !state)
		return NULL;
	for (size_t i = 0; i < sizeof(states_nl) / sizeof(states_nl[0]); i++)
		if (!strcmp(states_nl[i][0], state))
			return states_nl[i][1];
	return NULL;
}

/* a connection state as ha.c and mqtt.c keep it, for the screen */
const char *status_text(const char *s, char *buf, size_t len)
{
	int n;

	if (lang_nl && s && sscanf(s, "Connected, %d values found", &n) == 1) {
		snprintf(buf, len, "Verbonden, %d waarden gevonden", n);
		return buf;
	}
	if (lang_nl && s && sscanf(s, "Own broker, %d values found", &n) == 1) {
		snprintf(buf, len, "Eigen broker, %d waarden gevonden", n);
		return buf;
	}
	return tr(s);
}

/* "Monday 26 October", "maandag 26 oktober" */
void date_text(char *out, size_t len, const struct tm *tm)
{
	static const char *const days[] = { "zondag", "maandag", "dinsdag", "woensdag",
					    "donderdag", "vrijdag", "zaterdag" };
	static const char *const months[] = { "januari", "februari", "maart", "april", "mei", "juni",
					      "juli", "augustus", "september", "oktober", "november", "december" };

	if (!lang_nl) {
		strftime(out, len, "%A %e %B", tm);
		return;
	}
	snprintf(out, len, "%s %d %s", days[tm->tm_wday % 7], tm->tm_mday, months[tm->tm_mon % 12]);
}
