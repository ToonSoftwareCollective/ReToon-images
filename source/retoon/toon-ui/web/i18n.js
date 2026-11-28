/* The web interface in Dutch (the default) or English.
 * The page and app.js are written in English. Plain texts are translated as
 * they appear on the page (a MutationObserver looks them up in NL); texts
 * with values in them go through T`...` in app.js, whose keys hold {0},
 * {1}... for the values. Elements with translate="no" (names people gave,
 * release notes) are left alone. The language is the Toon's own setting
 * (Settings, System), so the screen speaks the same one. */
'use strict';
let LANG = 'nl';
try { LANG = localStorage.lang || 'nl'; } catch (e) { /* private window */ }
document.documentElement.lang = LANG;

const NL = {
  // header and tabs
  'Overview': 'Overzicht', 'Screen': 'Scherm', 'Thermostat': 'Thermostaat', 'Connections': 'Verbindingen',
  'Apps': 'Apps', 'System': 'Systeem', 'Undo changes': 'Wijzigingen ongedaan maken', 'Send to ReToon': 'Naar ReToon sturen',
  // overview
  "What's new": 'Wat is er nieuw', 'Getting started': 'Aan de slag', 'A few steps to a ReToon that runs on its own.':
    'Een paar stappen naar een ReToon die zelfstandig werkt.', 'Hide this list': 'Deze lijst verbergen', 'Right now': 'Nu',
  'Connect to Home Assistant': 'Verbind met Home Assistant', 'Set up the thermostat': 'Stel de thermostaat in',
  'Turn on "Always start ReToon"': 'Zet "Altijd ReToon starten" aan', 'Open': 'Openen', 'Version': 'Versie',
  'No signal': 'Geen signaal', 'Not connected': 'Niet verbonden', 'Log in under Connections': 'Log in onder Verbindingen',
  'Not set up': 'Niet ingesteld', 'Optional': 'Optioneel', 'In use': 'In gebruik', 'Waiting': 'Wacht',
  'For Z-Wave JS': 'Voor Z-Wave JS', 'Z-Wave JS connected': 'Z-Wave JS verbonden', 'for Z-Wave JS to connect': 'op Z-Wave JS',
  'For Home Assistant': 'Voor Home Assistant', 'for Home Assistant to connect': 'op Home Assistant', 'For zigbee2mqtt': 'Voor zigbee2mqtt',
  'strong signal': 'sterk signaal', 'fair signal': 'redelijk signaal', 'weak signal': 'zwak signaal',
  'heating': 'verwarmt', 'Heating now': 'Verwarmt', 'not heating': 'verwarmt niet', 'Not heating': 'Verwarmt niet', 'set by hand': 'met de hand',
  'an own build': 'een eigen build', 'Not running': 'Draait niet', 'Off': 'Uit',
  // screen tab
  'Pages': "Pagina's", '+ Page': '+ Pagina', 'Copy pages from Home Assistant…': "Pagina's kopiëren uit Home Assistant…",
  'Cards': 'Kaarten', '+ Group': '+ Groep', 'On the ReToon now': 'Nu op de ReToon', 'Click to tap on the ReToon': 'Klik om op de ReToon te tikken',
  "The ReToon's real screen; click in it to tap. Your changes appear after": 'Het echte scherm van de ReToon; klik erin om te tikken. Je wijzigingen verschijnen na',
  'at the top.': 'bovenaan.', 'Refresh': 'Vernieuwen', 'Copy pages from Home Assistant': "Pagina's kopiëren uit Home Assistant",
  'Pick one of your Home Assistant dashboards and choose which of its pages to copy. Copies are yours to change afterwards; nothing is changed in Home Assistant.':
    "Kies een van je dashboards in Home Assistant en welke pagina's je wilt kopiëren. De kopieën kun je daarna zelf aanpassen; in Home Assistant verandert niets.",
  'Show my dashboards': 'Mijn dashboards tonen', 'Close': 'Sluiten', 'Brightness': 'Helderheid',
  'Normal brightness (%)': 'Normale helderheid (%)', 'Dim after (seconds, 0 = never)': 'Dimmen na (seconden, 0 = nooit)',
  'Dimmed brightness (%)': 'Gedimde helderheid (%)', 'Screen off at night': 'Scherm uit in de nacht', 'Page behaviour': 'Gedrag van de pagina\'s', 'Other': 'Overig', 'Instead of dimming, the screen goes off after the dim time (half a minute when it does not dim). A touch wakes it.': 'In plaats van dimmen gaat het scherm na de dimtijd uit (na een halve minuut als het niet dimt). Een aanraking maakt het wakker.', 'Screen off while the thermostat is on Sleep': 'Scherm uit als de thermostaat op Slapen staat', 'Screen off from': 'Scherm uit van', 'until': 'tot', 'Grey instead of colour while dimmed': 'Grijs in plaats van kleur als het scherm gedimd is',
  'Go back to the home page when the screen is not touched': 'Terug naar de startpagina als het scherm niet wordt aangeraakt', 'Go back after (seconds)': 'Terug na (seconden)', 'Slide the pages when swiping': "Pagina's laten schuiven bij vegen",
  'Presets and week program under the thermostat': 'Standen en weekprogramma onder de thermostaat',
  'Off: the thermostat shows only the dial, and the week program is switched off. Set the temperature on the dial or from Home Assistant.':
    'Uit: de thermostaat toont alleen de draaiknop, en het weekprogramma gaat uit. De temperatuur stel je in met de draaiknop of vanuit Home Assistant.',
  'Home page (dimmed)': 'Startpagina (gedimd)', 'Different page when the screen is dimmed': 'Andere pagina als het scherm gedimd is',
  'Shown while the screen is dimmed (Screen, Brightness). A touch wakes the screen and shows the home page, without pressing anything.':
    'Te zien zolang het scherm gedimd is (Scherm, Helderheid). Een aanraking maakt het scherm wakker en toont de startpagina, zonder iets in te drukken.',
  'Remove the dimmed page and its cards?': 'De gedimde pagina en haar kaarten verwijderen?', 'Home page': 'Startpagina',
  'Touch screen': 'Aanraakscherm',
  "When taps land next to where you press: the Toon's screen shows four crosses to tap. The new calibration is only kept when you then tap Keep.":
    'Als een tik naast de plek terechtkomt waar je drukt: het scherm van de Toon toont vier kruisjes om op te tikken. De nieuwe kalibratie blijft alleen als je daarna op Bewaren tikt.',
  'Notifications': 'Meldingen',
  'Home Assistant, apps and scripts can show a message on the Toon, with a sound. A short message shows at the top and goes by itself; one with buttons or an alert stays in the middle until it is answered. See the documentation (MQTT) for how to send one.':
    'Home Assistant, apps en scripts kunnen een melding op de Toon tonen, met geluid. Een korte melding verschijnt bovenaan en verdwijnt vanzelf; een melding met knoppen of een alarm blijft in het midden staan tot er op gereageerd is. Hoe je er een stuurt staat in de documentatie (MQTT).',
  'Try a message': 'Melding proberen', 'Try one with buttons': 'Melding met knoppen proberen', 'Look at the Toon': 'Kijk op de Toon',
  'This is how a short message looks on the Toon.': 'Zo ziet een korte melding eruit op de Toon.',
  'A message with buttons stays until one is tapped.': 'Een melding met knoppen blijft staan tot er een wordt aangetikt.',
  'Later': 'Later', 'OK': 'OK',
  'Calibrate on the Toon': 'Kalibreren op de Toon', 'Back to the standard calibration': 'Terug naar de standaardkalibratie',
  "Tap the crosses on the Toon's screen": 'Tik op de kruisjes op het scherm van de Toon',
  "Forget the calibration made on this Toon? It goes back to the one from the factory, or to the screen's own.":
    'De kalibratie van deze Toon vergeten? Dan geldt weer die uit de fabriek, of die van het scherm zelf.',
  'Save': 'Opslaan', 'Standard pages': "Standaardpagina's", 'Replaces all your pages with the ones a new ReToon starts with.':
    "Vervangt al je pagina's door die waarmee een nieuwe ReToon begint.", "Put back the standard pages": "Standaardpagina's terugzetten",
  'Name': 'Naam', 'Icon': 'Pictogram', 'Columns on this page': 'Kolommen op deze pagina', 'Icon names come from': 'Namen van pictogrammen komen van',
  ', for example mdi:sofa.': ', bijvoorbeeld mdi:sofa.', 'Move up': 'Omhoog', 'Move down': 'Omlaag', 'Remove this page': 'Deze pagina verwijderen',
  'Add a page, or copy one with "Copy pages from Home Assistant".': 'Voeg een pagina toe, of kopieer er een met "Pagina\'s kopiëren uit Home Assistant".',
  'this page': 'deze pagina', 'Move group back': 'Groep naar voren', 'Move group on': 'Groep naar achteren', 'Remove this group': 'Deze groep verwijderen',
  'Remove this group and its cards?': 'Deze groep en de kaarten erin verwijderen?', 'Remove': 'Verwijderen', '+ Card': '+ Kaart',
  'Not saved yet': 'Nog niet opgeslagen', 'Saved': 'Opgeslagen', 'Home Assistant error': 'Fout van Home Assistant',
  'App · not installed': 'App · niet geïnstalleerd', 'not shown on the ReToon': 'niet te tonen op de ReToon',
  'Energy usage': 'Energieverbruik', 'Solar production': 'Zonne-opbrengst', 'Gas usage': 'Gasverbruik',
  'Energy distribution': 'Energieverdeling',
  'Shows how the energy of today flows between the grid, solar panels, battery and the house, with the gas and water used.':
    'Toont hoe de energie van vandaag loopt tussen het net, de zonnepanelen, de thuisbatterij en het huis, met het verbruikte gas en water.',
  'Shows today per hour.': 'Toont vandaag per uur.',
  ' and loses about {0} °C an hour without heating for every 10 °C it is colder outside':
    ' en verliest ongeveer {0} °C per uur zonder verwarming per 10 °C dat het buiten kouder is',
  ' (now {0} °C an hour).': ' (nu {0} °C per uur).',
 ' Starting early begins {0} minutes sooner than calculated, from {1} mornings.':
    ' Vooraf opwarmen begint {0} minuten eerder dan berekend, geleerd van {1} keer.',
  ' Starting early begins {0} minutes later than calculated, from {1} mornings.':
    ' Vooraf opwarmen begint {0} minuten later dan berekend, geleerd van {1} keer.',
 'Keep a measurement log (a line a minute)': 'Een meetlog bijhouden (een regel per minuut)',
  "For seeing how the thermostat works in your house, or to send to the developers: each minute the room and target temperature, the flow temperature asked for, whether it heats, what the installation reports (flow and return temperature, modulation, flame), the outside temperature and what the thermostat has learned. The last 2 to 3 days, as a CSV file for Excel; written every 10 minutes, so the last minutes may not be in it yet. Off by default.": "Om te zien hoe de thermostaat in jouw huis werkt, of om naar de ontwikkelaars te sturen: elke minuut de kamer- en gewenste temperatuur, de gevraagde aanvoertemperatuur, of hij verwarmt, wat de installatie meldt (aanvoer- en retourtemperatuur, modulatie, vlam), de buitentemperatuur en wat de thermostaat geleerd heeft. De laatste 2 tot 3 dagen, als CSV-bestand voor Excel; elke 10 minuten weggeschreven, dus de laatste minuten staan er mogelijk nog niet in. Standaard uit.",
  'Download the measurements': 'De metingen downloaden',
 ' After 10 minutes of heating the room still rises about {0} °C from the warm radiators; the thermostat stops earlier for that.':
    ' Na 10 minuten verwarmen loopt de kamer nog ongeveer {0} °C op door de warme radiatoren; daarvoor stopt de thermostaat eerder.',
  " Started from what the Toon's original software had learned.": ' Begonnen met wat de originele software van de Toon had geleerd.',
  'Standard': 'Standaard', 'Large': 'Groot', 'Name: text size': 'Naam: tekstgrootte', 'Name: colour': 'Naam: kleur', 'State: text size': 'Status: tekstgrootte',
  'State: colour': 'Status: kleur', 'Extra large': 'Extra groot', 'Own colour…': 'Eigen kleur…', 'White': 'Wit',
  'Grey': 'Grijs', 'Red': 'Rood', 'Orange': 'Oranje', 'Amber': 'Amber', 'Yellow': 'Geel', 'Green': 'Groen',
  'Teal': 'Blauwgroen', 'Cyan': 'Cyaan', 'Blue': 'Blauw', 'Purple': 'Paars', 'Pink': 'Roze',
  "Correction of the ReToon's own sensor (°C)": 'Correctie van de eigen sensor van de ReToon (°C)',
  'The ReToon measures {0} °C; with the correction {1} °C.': 'De ReToon meet {0} °C; met de correctie {1} °C.',
  'The ReToon measures {0} °C.': 'De ReToon meet {0} °C.',
  'The numbers come from the Energy dashboard of Home Assistant: the meters you chose there.':
    'De cijfers komen uit het Energie-dashboard van Home Assistant: de meters die je daar gekozen hebt.', 'Energy · from Home Assistant': 'Energie · uit Home Assistant',
  // card dialog
  'Card': 'Kaart', 'Cancel': 'Annuleren', 'OK': 'OK', 'Tile': 'Tegel', 'Heading': 'Kop', 'List of entities': 'Lijst van entiteiten',
  'Graph': 'Grafiek', 'Period': 'Periode', '1 hour': '1 uur', '3 hours': '3 uur', '6 hours': '6 uur', '12 hours': '12 uur',
  '24 hours': '24 uur', '2 days': '2 dagen', '3 days': '3 dagen', 'A week': 'Een week',
  'Numbers only. Lines with different units get a graph each.': 'Alleen getallen. Lijnen met verschillende eenheden krijgen elk een eigen grafiek.',
  'Gauge': 'Meter', 'Minimum': 'Minimum', 'Maximum': 'Maximum', 'Unit (empty = its own)': 'Eenheid (leeg = de eigen eenheid)',
  'Needle': 'Naald', 'Green from': 'Groen vanaf', 'Yellow from': 'Geel vanaf', 'Red from': 'Rood vanaf',
  'Colours: the value from which the ring turns green, yellow or red. Leave them empty for a blue ring.':
    'Kleuren: de waarde vanaf waar de ring groen, geel of rood wordt. Laat ze leeg voor een blauwe ring.',
  'This card has its own colour segments (from Home Assistant). Filling in a colour here replaces them.':
    'Deze kaart heeft eigen kleurvlakken (uit Home Assistant). Een kleur hier invullen vervangt die.',
  'Text': 'Tekst', 'Clock': 'Klok', 'App': 'App', 'None': 'Geen', 'Brightness slider': 'Schuif voor helderheid', 'Volume slider': 'Schuif voor volume', 'Position slider': 'Schuif voor positie', 'Open / stop / close buttons': 'Knoppen open / stop / dicht',
  'Buttons under the dial': 'Knoppen onder de draaiknop', 'Buttons under the thermostat': 'Knoppen onder de thermostaat', 'With the buttons comes the week program button. None: only the temperature, set with − and + (or from Home Assistant).': 'Met de knoppen komt ook de knop van het weekprogramma. Geen: alleen de temperatuur, in te stellen met − en + (of vanuit Home Assistant).', 'Automatic (presets, else modes)': 'Automatisch (voorkeuzes, anders verwarmingsmodi)', 'Colour when on': 'Kleur als hij aan is', 'For this card only (the other settings are under Apps):': 'Alleen voor deze kaart (de andere instellingen staan onder Apps):',
  'Temperature − / +': 'Temperatuur − / +', 'Option buttons': 'Knoppen met opties', 'Heating mode buttons': 'Knoppen voor verwarmingsmodus',
  'Full width': 'Volle breedte', 'Half': 'Half', 'One third': 'Een derde', 'One quarter': 'Een kwart', 'Search by name…': 'Zoek op naam…',
  'Nothing found. You can also type an entity id.': 'Niets gevonden. Je kunt ook een entiteit-id typen.',
  'Card settings as Home Assistant writes them.': 'Kaartinstellingen zoals Home Assistant ze schrijft.', 'Kind of card': 'Soort kaart',
  'Shows': 'Toont', 'Name (empty = its own name)': 'Naam (leeg = de eigen naam)', 'Large dial (fills its section)': 'Grote draaiknop (vult de groep)',
  'Small dial': 'Kleine draaiknop', 'Classic (no dial, presets two a row)': 'Klassiek (zonder draaiknop, standen twee per rij)', 'Bar with a slider': 'Balk met schuif', 'One line with − and +': 'Eén regel met − en +', 'Style': 'Stijl', 'Digital': 'Digitaal', 'Analogue (hands)': 'Analoog (wijzers)', 'Seconds hand': 'Secondewijzer',
  'Hands: colour': 'Wijzers: kleur', 'Hour marks: colour': 'Uurstreepjes: kleur', 'Minute marks: colour': 'Minuutstreepjes: kleur',
  'Dial: colour (the others follow it)': 'Wijzerplaat: kleur (de rest volgt deze)', 'Black background': 'Zwarte achtergrond',
  'Icon (empty = automatic)': 'Pictogram (leeg = automatisch)', 'Width': 'Breedte', 'Choose…': 'Kies…', 'No apps installed': 'Geen apps geïnstalleerd',
  'Preset buttons': 'Knoppen voor voorkeuzes', 'Fan speed buttons': 'Knoppen voor ventilatorstand', 'Swing buttons': 'Knoppen voor zwenken', 'Fan preset buttons': 'Knoppen voor ventilatorstanden', 'Fan speed slider': 'Schuif voor ventilatorsnelheid', 'Buttons (none ticked = all, at most 5)': 'Knoppen (niets aangevinkt = allemaal, hooguit 5)',
  'Buttons (as the entity calls them, separated by commas; empty = all)': 'Knoppen (zoals de entiteit ze noemt, met komma\'s ertussen; leeg = allemaal)',
  'At most {0} buttons fit.': 'Er passen hooguit {0} knoppen.', 'At most {0} pages fit on the Toon.': 'Er passen hooguit {0} pagina\'s op de Toon.', 'When tapped': 'Bij aantikken',
  'Its usual thing (switch, press...)': 'Het gewone (schakelen, indrukken...)', 'Nothing': 'Niets', 'Perform an action': 'Een actie uitvoeren',
  'Action (domain.action)': 'Actie (domein.actie)', 'On entity (empty = none)': 'Op entiteit (leeg = geen)',
  'Data, as JSON (empty = none)': 'Gegevens, als JSON (leeg = geen)', 'Ask first': 'Eerst vragen', 'Question': 'Vraag',
  'auto': 'auto', 'Drag to move': 'Sleep om te verplaatsen', 'Automatic size': 'Automatische grootte',
  'Drag to change the size': 'Sleep om de grootte te veranderen',
  "The Toon's screen ends at this line; what is below it is seen by scrolling on the Toon.": 'Bij deze lijn eindigt het scherm van de Toon; wat eronder staat zie je door op de Toon te scrollen.',
  'Drag a card to any free place, or its corner to make it larger or smaller. Click a card to change it, or an empty cell to add one there. The size is columns × rows of the page\'s 12 columns.':
    'Sleep een kaart naar een vrije plek, of zijn hoek om hem groter of kleiner te maken. Klik op een kaart om hem te veranderen, of op een leeg vak om er daar een toe te voegen. De grootte is kolommen × rijen van de 12 kolommen van de pagina.',
  'Add a card here': 'Hier een kaart toevoegen',
  'Update by itself': 'Vanzelf bijwerken', 'Update apps by themselves': 'Apps vanzelf bijwerken',
  'every hour': 'elk uur', 'every night around 3:00': 'elke nacht rond 3:00', 'every week on Sunday night': 'elke week in de nacht naar zondag',
  'Uses the same schedule as the update of ReToon itself (System, Update and restart): now {0}. The screen restarts after an app is updated, and then says which ones.':
    'Volgt hetzelfde schema als het bijwerken van ReToon zelf (Systeem, Bijwerken en herstarten): nu {0}. Het scherm herstart nadat een app is bijgewerkt en meldt dan welke.',
  'Uses the same schedule as the update of ReToon itself (System, Update and restart), which is now set to Never: then apps are not updated by themselves either.':
    'Volgt hetzelfde schema als het bijwerken van ReToon zelf (Systeem, Bijwerken en herstarten), en dat staat nu op Nooit: dan worden ook apps niet vanzelf bijgewerkt.', 'Never': 'Nooit', 'Every hour': 'Elk uur',
  'Every night (around 3:00)': 'Elke nacht (rond 3:00)', 'Every week (Sunday night)': 'Elke week (zondagnacht)',
  'An update that is found is installed, and the ReToon then restarts by itself. The screen says so afterwards.':
    'Een gevonden update wordt geïnstalleerd, en de ReToon herstart daarna vanzelf. Het scherm meldt dat achteraf.',
  'Meter adapters': 'Meteradapters', 'Gas': 'Gas', 'Gas flow': 'Gasverbruik nu', 'Heat': 'Warmte',
  'Electricity import (meter sensor)': 'Stroom afgenomen (metersensor)', 'Electricity export (meter sensor)': 'Stroom teruggeleverd (metersensor)',
  'Electricity import tariff 1': 'Stroom afgenomen tarief 1', 'Electricity import tariff 2': 'Stroom afgenomen tarief 2',
  'Electricity export tariff 1': 'Stroom teruggeleverd tarief 1', 'Electricity export tariff 2': 'Stroom teruggeleverd tarief 2',
  '{0} power': '{0}, vermogen', 'Use the meter adapter at node {0}': 'Meteradapter op node {0} gebruiken', ', now {0}': ', nu {0}',
  'None seen yet. A meter adapter shows up here when it sends a reading, every 10 seconds while a value changes.':
    'Nog geen gezien. Een meteradapter verschijnt hier zodra hij een meting stuurt, elke 10 seconden zolang een waarde verandert.',
  'Turn on the Z-Wave radio above: meter adapters are found while it is on.':
    'Zet hierboven de Z-Wave-radio aan: meteradapters worden gevonden zolang die aan staat.',
  'Z-Wave JS does not support the readings of these meter adapters yet, so ReToon reads them itself, from what passes through the radio.':
    'Z-Wave JS ondersteunt de metingen van deze meteradapters nog niet, daarom leest ReToon ze zelf mee met wat er via de radio binnenkomt.',
  'Tick an adapter to use its readings in ReToon: on the screen, and over MQTT in Home Assistant. Untick it once Z-Wave JS supports them, or Home Assistant gets them twice.':
    'Vink een adapter aan om zijn metingen in ReToon te gebruiken: op het scherm en via MQTT in Home Assistant. Zet het vinkje uit zodra Z-Wave JS ze ondersteunt, anders krijgt Home Assistant ze dubbel.',
  'Are you sure?': 'Weet je het zeker?',
  'Give the action as domain.action, for example script.turn_on.': 'Geef de actie als domein.actie, bijvoorbeeld script.turn_on.',
  'Extra control': 'Extra bediening', 'Hide the state text': 'Statustekst verbergen', 'Show the name': 'De naam tonen', 'Title': 'Titel', 'Small': 'Klein', 'Size': 'Grootte',
  'Add': 'Toevoegen', 'Simple form': 'Eenvoudig formulier', 'Edit as text (advanced)': 'Als tekst bewerken (gevorderd)', 'New card': 'Nieuwe kaart',
  'Choose what the card shows first.': 'Kies eerst wat de kaart toont.', 'Choose an app first.': 'Kies eerst een app.',
  // import
  'Asking Home Assistant…': 'Home Assistant vragen…', 'Overview (the default dashboard)': 'Overzicht (het standaarddashboard)', 'Loading…': 'Laden…',
  'This dashboard is generated by Home Assistant itself and has no pages to copy. Use "Take control" on it in Home Assistant first, or pick another one.':
    "Dit dashboard maakt Home Assistant zelf en heeft geen pagina's om te kopiëren. Gebruik er eerst \"Beheer overnemen\" op in Home Assistant, of kies een ander.",
  'Copied. Check the pages, then press Send to ReToon.': "Gekopieerd. Controleer de pagina's en druk dan op Naar ReToon sturen.",
  'Dashboard': 'Dashboard', 'Add these pages': "Deze pagina's toevoegen", 'Replace all my pages with these': "Al mijn pagina's hierdoor vervangen",
  'Replace all pages on the ReToon with the selected ones?': "Alle pagina's op de ReToon vervangen door de gekozen pagina's?",
  // thermostat
  'Heating': 'Verwarming', 'Thermostat on': 'Thermostaat aan', 'Back to the program now': 'Nu terug naar het programma',
  'Off still protects against frost. On the ReToon itself you choose one of the four presets or turn the dial; that also switches the thermostat on.':
    'Uit beschermt nog steeds tegen vorst. Op de ReToon zelf kies je een van de vier standen of draai je aan de knop; dat zet de thermostaat ook aan.',
  'Presets': 'Standen', 'Comfort (°C)': 'Comfort (°C)', 'Home (°C)': 'Thuis (°C)', 'Sleep (°C)': 'Slapen (°C)', 'Away (°C)': 'Weg (°C)',
  'Temperature sensors': 'Temperatuursensoren', "The ReToon's own sensor": 'De eigen sensor van de ReToon',
  'Add a sensor from Home Assistant or MQTT': 'Een sensor uit Home Assistant of MQTT toevoegen', 'How the sensors are used': 'Hoe de sensoren worden gebruikt',
  "The thermostat works with the average of the sensors chosen here. If none of the others answers, it uses the ReToon's own sensor.":
    'De thermostaat werkt met het gemiddelde van de sensoren die je hier kiest. Als geen van de andere antwoordt, gebruikt hij de eigen sensor van de ReToon.',
  'Smart heating': 'Slim verwarmen', 'Start early, so a warmer step of the program is reached on time': 'Eerder beginnen, zodat een warmere stap van het programma op tijd bereikt is',
  'Start at most this many minutes early': 'Maximaal zoveel minuten eerder beginnen',
  "Choose the flow temperature from what the house needs (OpenTherm)": "De aanvoertemperatuur kiezen naar wat het huis nodig heeft (OpenTherm)",
  "Otherwise, this flow temperature (°C)": "Anders deze aanvoertemperatuur (°C)",
  'Warm up at most (°C an hour)': 'Maximaal zoveel opwarmen (°C per uur)',
  'Lower this for a slow heating system such as floor heating.': 'Zet dit lager voor een trage verwarming zoals vloerverwarming.',
  "On/off heating or district heating: cycle time (minutes)": "Aan/uit-verwarming of stadsverwarming: cyclustijd (minuten)",
  "OpenTherm, but the heat source does not follow the flow temperature asked for (district heating)": "OpenTherm, maar de warmtebron volgt de gevraagde aanvoertemperatuur niet (stadsverwarming)",
  "Switch this on when your heating is connected over OpenTherm but the flow temperature cannot be set, as with most district heating sets: hot water arrives whatever the thermostat asks for. You can tell by the room temperature: every time the heating stops, also close to the target, the room keeps getting warmer for a long while (it overshoots). With this on, the thermostat switches the heat on in short pulses, as for an on/off boiler, and learns how the house warms up that way. The other OpenTherm values (hot water, faults) stay. Leave it off for an ordinary gas boiler or a heat pump: those do follow the temperature.": "Zet dit aan als je verwarming via OpenTherm is aangesloten maar de aanvoertemperatuur niet in te stellen is, zoals bij de meeste afleversets van stadsverwarming: er komt heet water, wat de thermostaat ook vraagt. Je herkent het aan de kamertemperatuur: elke keer dat de verwarming stopt, ook vlak bij de gewenste temperatuur, blijft de kamer nog lang warmer worden (hij schiet door). Met dit aan schakelt de thermostaat de warmte in korte pulsen, zoals bij een aan/uit-ketel, en leert hij zo hoe snel het huis opwarmt. De andere OpenTherm-waarden (warm water, storingen) blijven. Laat het uit bij een gewone cv-ketel of een warmtepomp: die volgen de temperatuur wel.",
  "On/off heating is either fully on or off. So the thermostat switches it on for part of every cycle, as much as the house needs: for example 4 of the 20 minutes when the room is nearly warm enough, the whole cycle when it is still cold. When the house cools faster than expected, it comes on again within the same cycle. Shorter: a steadier temperature, but it starts more often. Longer: fewer starts, for a slow system such as floor heating. Standard 20.": "Aan/uit-verwarming staat helemaal aan of helemaal uit. De thermostaat zet hem daarom elke cyclus een deel van de tijd aan, zoveel als het huis nodig heeft: bijvoorbeeld 4 van de 20 minuten als het bijna warm genoeg is, de hele cyclus als het nog koud is. Koelt het huis sneller af dan verwacht, dan slaat hij in dezelfde cyclus opnieuw aan. Korter: een gelijkmatigere temperatuur, maar hij start vaker. Langer: minder starts, voor een trage verwarming zoals vloerverwarming. Standaard 20.",
  'Start heating above (°C)': 'Verwarming starten boven (°C)',
  "Heating starts when the flow temperature asked for is above this. Standard 30; lower it (to 25) for floor heating or a heat pump. An installation that cannot run that low may switch on and off often.": "De verwarming start als de gevraagde aanvoertemperatuur hierboven komt. Standaard 30; lager (tot 25) voor vloerverwarming of een warmtepomp. Een installatie die niet zo laag kan draaien, slaat dan mogelijk vaak aan en uit.",
  'Explanation': 'Uitleg',
  'Display brightness': 'Helderheid scherm',
  "Maximum flow temperature": "Maximale aanvoertemperatuur", "Highest flow temperature": "Hoogste aanvoertemperatuur",
  "The installation's own setting": "De instelling van de installatie zelf", 'Fixed': 'Vast', 'By the outside temperature': 'Op de buitentemperatuur',
  'Maximum (°C)': 'Maximaal (°C)', 'Outside (°C)': 'Buiten (°C)', "Flow at most (°C)": "Aanvoer maximaal (°C)",
  'Between these two points the maximum follows a straight line; colder or warmer, it stays at the end value. Without an outside temperature the cold value is used.': 'Tussen deze twee punten volgt het maximum een rechte lijn; kouder of warmer blijft het op de eindwaarde. Zonder buitentemperatuur geldt de koude waarde.',
  "For OpenTherm, for example floor heating without its own pump: the flow never gets warmer than this, and never warmer than the installation's own maximum. At least 2 °C above where heating starts (Smart heating).": "Voor OpenTherm, bijvoorbeeld vloerverwarming zonder eigen pomp: de aanvoer wordt nooit warmer dan dit, en nooit warmer dan het maximum van de installatie zelf. Minstens 2 °C boven waar het verwarmen begint (Slim verwarmen).",
  'Outside temperature': 'Buitentemperatuur', 'Maximum now': 'Maximum nu', 'not known': 'niet bekend',
  "Only used when the installation does not give an outside temperature over OpenTherm. For the maximum flow temperature by the outside temperature, and for switching to cooling automatically.": "Wordt alleen gebruikt als de installatie geen buitentemperatuur via OpenTherm geeft. Voor de maximale aanvoertemperatuur op de buitentemperatuur, en om automatisch naar koelen te schakelen.",
  "With the box above off: the installation heats with this flow temperature, and the thermostat only switches the heating on and off. Empty: it follows how far the room is from the target.": "Met het vakje hierboven uit: de installatie verwarmt met deze aanvoertemperatuur en de thermostaat zet de verwarming alleen aan en uit. Leeg: hij volgt hoe ver de kamer van de gewenste temperatuur af is.",
  'Frost protection below (°C)': 'Vorstbeveiliging onder (°C)', 'Week program': 'Weekprogramma', 'All days the same': 'Alle dagen gelijk', 'Off still protects against frost.': 'Uit beschermt nog steeds tegen vorst.', 'How this works': 'Hoe dit werkt', 'Cooling': 'Koelen', 'Use cooling': 'Koelen gebruiken', 'Heating or cooling': 'Verwarmen of koelen', 'Heating': 'Verwarmen', 'Automatic, by the outside temperature': 'Automatisch, op de buitentemperatuur', 'Start cooling above outside (°C)': 'Gaan koelen boven buiten (°C)', 'Back to heating below outside (°C)': 'Terug naar verwarmen onder buiten (°C)', 'It switches after half an hour past the limit.': 'Hij schakelt na een half uur voorbij de grens.', 'Outside temperature sensor': 'Sensor voor de buitentemperatuur', 'sensor': 'sensor', 'Cooling temperatures of the presets': 'Koeltemperaturen van de standen', 'Outside': 'Buiten', 'Now on': 'Staat nu op', "Automatic needs an outside temperature: from the installation, or choose a sensor on the Installation page.": "Automatisch heeft een buitentemperatuur nodig: van de installatie, of kies een sensor op de pagina Installatie.", 'Hot water': 'Warm water', 'Temperature (°C)': 'Temperatuur (°C)', "Leave this empty to keep the installation's own setting.": "Laat dit leeg om de instelling van de installatie te houden.", 'Keep the tap water warm': 'Kraanwater warmhouden', 'Off: the boiler does not keep its tap water at temperature, and heats it only when a tap opens (with a combi boiler: a little longer until the water is warm). Not every boiler supports this; with some, off means no hot water at all.': 'Uit: de ketel houdt het kraanwater niet op temperatuur en verwarmt het pas als er een kraan opengaat (bij een combiketel: iets langer wachten op warm water). Niet elke ketel ondersteunt dit; bij sommige betekent uit helemaal geen warm water.', 'Follow the schedule': 'Het schema volgen', 'Each step sets the temperature until the next step.': 'Elke stap stelt de temperatuur in tot de volgende stap.', 'Now': 'Nu', 'Set to': 'Ingesteld', 'Next': 'Straks', '{0} °C, within a minute': '{0} °C, binnen een minuut', 'State': 'Status', 'Active': 'Actief', 'Idle': 'Rust', "Limits of the installation": "Grenzen installatie", 'Follow the week program': 'Het weekprogramma volgen',
  'A preset or temperature chosen by hand holds until the next step of the program.': 'Een stand of temperatuur die je met de hand kiest, geldt tot de volgende stap van het programma.',
  'Save thermostat settings': 'Thermostaatinstellingen opslaan', 'Comfort': 'Comfort', 'Home': 'Thuis', 'Sleep': 'Slapen', 'Away': 'Weg',
  'Monday': 'Maandag', 'Tuesday': 'Dinsdag', 'Wednesday': 'Woensdag', 'Thursday': 'Donderdag', 'Friday': 'Vrijdag', 'Saturday': 'Zaterdag', 'Sunday': 'Zondag',
  '+ Step': '+ Stap', 'Same for all days': 'Hetzelfde voor alle dagen', 'The thermostat is not running': 'De thermostaat draait niet',
  'Frost protection': 'Vorstbeveiliging', 'On, not heating now': 'Aan, verwarmt nu niet', 'Room': 'Kamer', 'Target': 'Doel', 'Program': 'Programma',
  'Boiler': 'Ketel', 'OpenTherm': 'OpenTherm', 'on/off': 'aan/uit', 'no boiler module': 'geen ketelmodule', 'No boiler module': 'Geen ketelmodule',
  'no reading': 'geen meting', ' (set by hand)': ' (met de hand)', "(the ReToon's own sensor: the others do not answer)": '(de eigen sensor van de ReToon: de andere antwoorden niet)',
  // connections
  'Address': 'Adres', 'Log in to Home Assistant': 'Inloggen bij Home Assistant', 'Disconnect': 'Verbinding verbreken',
  'Use a token you made yourself': 'Een token gebruiken dat je zelf maakte', 'Paste a long-lived access token you made in your Home Assistant profile.':
    'Plak een langlevend toegangstoken dat je in je Home Assistant-profiel hebt gemaakt.', 'Token': 'Token', 'Save token': 'Token opslaan',
  'https and certificates': 'https en certificaten', 'Check the certificate (https with a certificate from a public authority only)':
    'Het certificaat controleren (alleen voor https met een certificaat van een publieke uitgever)', 'What logging in does': 'Wat inloggen doet',
  'Logging in makes an access token for this ReToon in Home Assistant. It stays on the ReToon and you can withdraw it in your Home Assistant profile. Both http:// and https:// addresses work.':
    'Inloggen maakt in Home Assistant een toegangstoken voor deze ReToon. Het blijft op de ReToon en je kunt het intrekken in je Home Assistant-profiel. Adressen met http:// en https:// werken allebei.',
  'Checking…': 'Controleren…', 'Broker': 'Broker', 'User name': 'Gebruikersnaam', 'Password': 'Wachtwoord', 'Port and discovery prefix': 'Poort en discovery-prefix',
  'Port': 'Poort', 'Discovery prefix': 'Discovery-prefix', 'What MQTT does here': 'Wat MQTT hier doet',
  "Publishes the ReToon's sensors, heating and thermostat to Home Assistant (auto discovery). Devices that announce themselves on this broker (zigbee2mqtt, Tasmota and others) can be put on your pages too: look for \"(MQTT)\" when you add a card.": "Publiceert de sensoren, verwarming en thermostaat van de ReToon naar Home Assistant (auto discovery). Apparaten die zich op deze broker aanmelden (zigbee2mqtt, Tasmota en andere) kun je ook op je pagina's zetten: zoek naar \"(MQTT)\" als je een kaart toevoegt.",
  'Your own MQTT values': 'Je eigen MQTT-waarden', 'For topics that do not announce themselves. Each one becomes a value you can put on a page.':
    'Voor topics die zich niet zelf aanmelden. Elk wordt een waarde die je op een pagina kunt zetten.', '+ Value': '+ Waarde', 'Save values': 'Waarden opslaan',
  'Own broker': 'Eigen broker', 'This ReToon\'s own broker': 'De eigen broker van deze ReToon', 'Open it to devices on your network': 'Openzetten voor apparaten in je netwerk',
  'Running, open to the network': 'Actief, open voor het netwerk', 'Running, only for this ReToon': 'Actief, alleen voor deze ReToon',
  'What it is for': 'Waar die voor is', 'The network needs a user name and a password': 'Voor het netwerk zijn een gebruikersnaam en wachtwoord nodig',
  'Use letters, digits and . _ - in the user name': 'Gebruik letters, cijfers en . _ - in de gebruikersnaam',
  'The ReToon has its own MQTT broker. The programs and apps on the ReToon use it among themselves; on its own it cannot be reached from the network. Opened to the network, your devices and scripts can use it too, with the user name and password above, on port 1883. The ReToon\'s values are under retoon/ (for example retoon/climate), and retoon/boiler/set takes thermostat commands such as "target 20.5". Without a broker filled in above, the screen shows the devices that announce themselves on this one.':
    'De ReToon heeft een eigen MQTT-broker. De programma\'s en apps op de ReToon gebruiken die onderling; uit zichzelf is hij niet bereikbaar vanaf het netwerk. Open voor het netwerk kunnen je apparaten en scripts hem ook gebruiken, met de gebruikersnaam en het wachtwoord hierboven, op poort 1883. De waarden van de ReToon staan onder retoon/ (bijvoorbeeld retoon/climate), en retoon/boiler/set neemt thermostaatopdrachten aan zoals "target 20.5". Is hierboven geen broker ingevuld, dan toont het scherm de apparaten die zich op deze aanmelden.',
  'Let Z-Wave JS in Home Assistant use the Z-Wave radio of this ReToon': 'Z-Wave JS in Home Assistant de Z-Wave-radio van deze ReToon laten gebruiken',
  'Only from this address (empty = anyone on your network)': 'Alleen vanaf dit adres (leeg = iedereen in je netwerk)', 'address of Home Assistant': 'adres van Home Assistant',
  'How to use this in Home Assistant': 'Zo gebruik je dit in Home Assistant',
  "In Home Assistant, add the Z-Wave integration (Z-Wave JS) and give it the address shown above instead of a USB stick. Devices that are already in this ReToon's Z-Wave network stay in it. The connection has no password and is not encrypted: fill in the address of Home Assistant so nothing else can use it.":
    'Voeg in Home Assistant de Z-Wave-integratie (Z-Wave JS) toe en geef het adres hierboven op in plaats van een USB-stick. Apparaten die al in het Z-Wave-netwerk van deze ReToon zitten, blijven erin. De verbinding heeft geen wachtwoord en is niet versleuteld: vul het adres van Home Assistant in, zodat niets anders hem kan gebruiken.',
  'Network keys of the original software': 'Netwerksleutels van de originele software',
  "Devices that were added with encryption only talk to Z-Wave JS when it has these keys: fill them in under the Z-Wave integration's security keys (S0 legacy, S2 unauthenticated, authenticated, access control).":
    'Apparaten die met versleuteling zijn toegevoegd, praten alleen met Z-Wave JS als het deze sleutels heeft: vul ze in bij de beveiligingssleutels van de Z-Wave-integratie (S0 legacy, S2 unauthenticated, authenticated, access control).',
  'Bluetooth proxy': 'Bluetooth-proxy', 'Let Home Assistant use the Bluetooth radio of this ReToon': 'Home Assistant de Bluetooth-radio van deze ReToon laten gebruiken',
  'The ReToon presents itself to Home Assistant as an ESPHome Bluetooth proxy. Home Assistant finds it by itself: under Settings, Devices & services it appears as a discovered ESPHome device; press Add. Bluetooth devices near this ReToon (thermometers, plant sensors, trackers, Shelly BLU, SwitchBot...) then work in Home Assistant as if they were next to it. Scanning only runs while Home Assistant is connected. The connection has no password and is not encrypted: fill in the address of Home Assistant so nothing else can use it.':
    'De ReToon meldt zich bij Home Assistant als ESPHome Bluetooth-proxy. Home Assistant vindt hem zelf: onder Instellingen, Apparaten en diensten verschijnt hij als ontdekt ESPHome-apparaat; druk op Toevoegen. Bluetooth-apparaten in de buurt van deze ReToon (thermometers, plantsensoren, trackers, Shelly BLU, SwitchBot...) werken dan in Home Assistant alsof ze ernaast staan. Er wordt alleen gescand zolang Home Assistant verbonden is. De verbinding heeft geen wachtwoord en is niet versleuteld: vul het adres van Home Assistant in, zodat niets anders hem kan gebruiken.',
  'Zigbee or Thread': 'Zigbee of Thread', 'The ReToon has one radio for Zigbee and Thread; it does one of the two.': 'De ReToon heeft één radio voor Zigbee en Thread; hij doet een van de twee.',
  'Zigbee, for zigbee2mqtt': 'Zigbee, voor zigbee2mqtt', 'Thread border router, for Matter devices': 'Thread border router, voor Matter-apparaten',
  'How to use this in zigbee2mqtt': 'Zo gebruik je dit in zigbee2mqtt',
  'In zigbee2mqtt, set the adapter to "zoh" and the port to the address shown above.': 'Zet in zigbee2mqtt de adapter op "zoh" en de poort op het adres hierboven.',
  'The ReToon starts a Thread network of its own. In Home Assistant, add the OpenThread Border Router integration with the address shown above, then make this network the preferred one under Thread. Matter devices that use Thread can then be added in Home Assistant.':
    'De ReToon start een eigen Thread-netwerk. Voeg in Home Assistant de integratie OpenThread Border Router toe met het adres hierboven en maak dit netwerk dan het voorkeursnetwerk onder Thread. Matter-apparaten die Thread gebruiken, kun je daarna in Home Assistant toevoegen.',
  'Join another Thread network': 'Bij een ander Thread-netwerk aansluiten', 'Its dataset (hex, from the other border router)': 'De dataset ervan (hex, van de andere border router)',
  'Join this network': 'Bij dit netwerk aansluiten', 'Start a new network': 'Een nieuw netwerk starten',
  "WiFi is changed on the ReToon's own screen: tap the WiFi icon in the corner. It also appears by itself when the ReToon has no connection.":
    'WiFi wijzig je op het scherm van de ReToon zelf: tik op het WiFi-pictogram in de hoek. Het verschijnt ook vanzelf als de ReToon geen verbinding heeft.',
  'Connected': 'Verbonden', 'Not connected yet': 'Nog niet verbonden', 'Connecting': 'Verbinden', 'Account': 'Account', 'Uses': 'Gebruikt',
  'a token you pasted yourself': 'een token dat je zelf hebt geplakt',
  'Paste the token first': 'Plak eerst het token',
  'That is not an access token: copy it again from Home Assistant': 'Dat is geen toegangstoken: kopieer het opnieuw uit Home Assistant',
  'Home Assistant refused this token': 'Home Assistant weigert dit token',
  'Home Assistant is not reachable at this address': 'Home Assistant is niet bereikbaar op dit adres', ', made by logging in': ', gemaakt door in te loggen', 'Certificate': 'Certificaat',
  'checked': 'gecontroleerd', 'not checked': 'niet gecontroleerd', 'In use by Z-Wave JS': 'In gebruik door Z-Wave JS', 'Waiting for Z-Wave JS': 'Wacht op Z-Wave JS',
  'Client': 'Client', 'In use by Home Assistant': 'In gebruik door Home Assistant', 'Waiting for Home Assistant': 'Wacht op Home Assistant',
  'No Bluetooth radio': 'Geen Bluetooth-radio', 'Scanning': 'Scannen', 'no: the radio is busy (another program scans)': 'nee: de radio is bezig (een ander programma scant)',
  'no': 'nee', 'Not ready: the sound driver is not running': 'Niet klaar: het geluidsstuurprogramma draait niet',
  'Enter the address of Home Assistant': 'Vul het adres van Home Assistant in', 'Disconnected': 'Verbinding verbroken',
  'Running, leader of the network': 'Actief, leider van het netwerk', 'Running, router in the network': 'Actief, router in het netwerk',
  'Joined as a child': 'Aangesloten als child', 'Looking for the network': 'Zoekt het netwerk', 'Starting': 'Starten', 'Zigbee': 'Zigbee', 'Thread': 'Thread',
  'Network': 'Netwerk', 'Devices': 'Apparaten', 'Paste the dataset as hex': 'Plak de dataset als hex', 'Joining the network': 'Aansluiten bij het netwerk',
  'Start a new Thread network? Devices in the current one stay there and have to be added again.': 'Een nieuw Thread-netwerk starten? Apparaten in het huidige blijven daar en moeten opnieuw worden toegevoegd.',
  'New network started': 'Nieuw netwerk gestart', 'Signal': 'Signaal', 'Found': 'Gevonden', 'Value': 'Waarde', 'On / off (shown only)': 'Aan / uit (alleen tonen)',
  'Switch': 'Schakelaar', 'New value': 'Nieuwe waarde', 'Kind': 'Soort', 'Topic': 'Topic', 'Field, when the message is JSON': 'Veld, als het bericht JSON is',
  'Text that means on': 'Tekst die aan betekent', 'Text that means off': 'Tekst die uit betekent', 'Unit': 'Eenheid', 'Topic to switch it': 'Topic om te schakelen',
  'Every value needs a topic': 'Elke waarde heeft een topic nodig', 'Not configured': 'Niet ingesteld', 'Connection lost': 'Verbinding verbroken',
  'Home Assistant refused the login': 'Home Assistant weigerde de login', 'Bad Home Assistant address': 'Ongeldig adres van Home Assistant',
  'Home Assistant is not reachable': 'Home Assistant is niet bereikbaar', 'The Toon tries': 'De Toon probeert',
  'The Toon itself cannot reach {0}, even if your browser can reach Home Assistant. Check that this is the address of Home Assistant on your own network (not localhost, not the guest network) and that nothing on that machine blocks the Toon.':
    'De Toon zelf kan {0} niet bereiken, ook al kan je browser Home Assistant wel bereiken. Controleer of dit het adres van Home Assistant in je eigen netwerk is (niet localhost, niet het gastnetwerk) en of niets op die machine de Toon tegenhoudt.', 'The certificate of Home Assistant is not trusted': 'Het certificaat van Home Assistant wordt niet vertrouwd',
  'No secure connection to Home Assistant': 'Geen beveiligde verbinding met Home Assistant', 'That address is not Home Assistant': 'Dat adres is geen Home Assistant',
  'Broker not reachable': 'Broker niet bereikbaar',
  // apps
  'Apps installed on this ReToon and their settings. To show an app on the screen, add an "App" card to a page under Screen.':
    'Apps op deze ReToon en hun instellingen. Om een app op het scherm te tonen, zet je een kaart "App" op een pagina onder Scherm.',
  'Available': 'Beschikbaar', 'Apps from the ReToon app catalogue. The screen restarts after an app is installed, updated or removed.':
    'Apps uit de ReToon-appcatalogus. Het scherm herstart nadat een app is geïnstalleerd, bijgewerkt of verwijderd.',
  'Installing…': 'Installeren…', 'Updating…': 'Bijwerken…', 'Removing…': 'Verwijderen…', '. The screen restarts.': '. Het scherm herstart.',
  'No apps are installed on this ReToon.': 'Er zijn geen apps geïnstalleerd op deze ReToon.', 'Needs a newer ReToon': 'Heeft een nieuwere ReToon nodig',
  'This app has no settings.': 'Deze app heeft geen instellingen.', 'Every app in the catalogue is installed.': 'Elke app uit de catalogus is geïnstalleerd.',
  'Install': 'Installeren',
  // system
  'Update and restart': 'Bijwerken en herstarten', 'Check for update': 'Zoeken naar update', 'Restart': 'Herstarten', 'Where updates come from': 'Waar updates vandaan komen',
  '"Check for update" looks for a newer ReToon release (ToonSoftwareCollective/ReToon-images on GitHub) and shows what changed in it. An installed update is used from the next restart.':
    '"Zoeken naar update" zoekt een nieuwere ReToon-release (ToonSoftwareCollective/ReToon-images op GitHub) en toont wat er veranderd is. Een geïnstalleerde update wordt gebruikt vanaf de volgende herstart.',
  'Starting up': 'Opstarten', 'Always start ReToon': 'Altijd ReToon starten',
  'While this is off, the next restart starts the original Toon software again, and it stays there: ReToon then has to be started from that system once more. Turn it on once ReToon works the way you want.':
    'Zolang dit uit staat, start de volgende herstart weer de originele Toon-software, en daar blijft hij: ReToon moet dan opnieuw vanuit dat systeem worden gestart. Zet dit aan zodra ReToon werkt zoals je wilt.',
  'Name of this ReToon': 'Naam van deze ReToon', 'Name on the network': 'Naam in het netwerk', 'About the name': 'Over de naam',
  'Small letters, digits and "-". The ReToon is then found at https://<name>.local/ and makes a new certificate, so the browser warns once more. Your router shows the new name after the next restart. Empty puts the standard name back.':
    'Kleine letters, cijfers en "-". De ReToon is dan te vinden op https://<naam>.local/ en maakt een nieuw certificaat, dus de browser waarschuwt nog één keer. Je router toont de nieuwe naam na de volgende herstart. Leeg zet de standaardnaam terug.',
  'Sound': 'Geluid', 'Volume (%)': 'Volume (%)', 'Play test sound': 'Testgeluid afspelen', 'Sounds from the command line': 'Geluiden vanaf de opdrachtregel', 'and': 'en',
  'play sounds at this volume. The Toon 1 has a small speaker; the Toon 2 plays through its buzzer, which sounds thinner and is loudest for high tones.':
    'spelen geluid af op dit volume. De Toon 1 heeft een kleine luidspreker; de Toon 2 speelt via zijn zoemer, die dunner klinkt en het hardst is bij hoge tonen.',
  'Help': 'Help', 'How to use ReToon (in English)': 'Hoe ReToon werkt (in het Engels)',
  'Language': 'Taal', 'Language of this page and of the ReToon\'s screen. The screen restarts in the new language.':
    'Taal van deze pagina en van het scherm van de ReToon. Het scherm herstart in de nieuwe taal.',
  'Security': 'Beveiliging', 'Login for this web interface': 'Inloggen op deze webinterface', 'New password': 'Nieuw wachtwoord', 'New password again': 'Nieuw wachtwoord nogmaals',
  'Change password': 'Wachtwoord wijzigen', 'The password is also shown on the ReToon itself (tap the Home Assistant icon).': 'Het wachtwoord staat ook op de ReToon zelf (tik op het Home Assistant-pictogram).',
  'Password for SSH (user root)': 'Wachtwoord voor SSH (gebruiker root)',
  'It cannot be read back: if you lose it, the only way to get in again is "Factory defaults" below.': 'Het is niet terug te lezen: ben je het kwijt, dan kom je alleen weer binnen via "Fabrieksinstellingen" hieronder.',
  'You have set your own password.': 'Je hebt een eigen wachtwoord ingesteld.', 'The password is still the standard one, "toon".': 'Het wachtwoord is nog het standaardwachtwoord, "toon".',
  'Encryption': 'Versleuteling', 'Also allow plain http (not encrypted)': 'Ook gewoon http toestaan (niet versleuteld)', 'About the certificate': 'Over het certificaat',
  'This page runs over https with a certificate this ReToon made itself, so your browser warns once.': 'Deze pagina werkt via https met een certificaat dat deze ReToon zelf maakte, dus je browser waarschuwt één keer.',
  'No certificate could be made, so https is off.': 'Er kon geen certificaat worden gemaakt, dus https staat uit.', 'Details': 'Details', 'Factory defaults': 'Fabrieksinstellingen',
  'Removes every setting of this ReToon: WiFi, Home Assistant, MQTT, your pages, the thermostat program, the name and both passwords. The ReToon restarts and shows its WiFi screen, as on its first start. The original Toon software is not touched.':
    "Verwijdert alle instellingen van deze ReToon: WiFi, Home Assistant, MQTT, je pagina's, het thermostaatprogramma, de naam en beide wachtwoorden. De ReToon herstart en toont zijn WiFi-scherm, zoals bij de eerste start. De originele Toon-software blijft ongemoeid.",
  'Back to factory defaults': 'Terug naar fabrieksinstellingen', 'The two passwords are not the same': 'De twee wachtwoorden zijn niet gelijk',
  'Changed. Your browser will ask for it.': 'Gewijzigd. Je browser vraagt erom.', 'Changed': 'Gewijzigd', 'Fill in a password': 'Vul een wachtwoord in',
  'Remove ALL settings of this ReToon and restart it? This cannot be undone.': 'ALLE instellingen van deze ReToon verwijderen en herstarten? Dit kan niet ongedaan worden gemaakt.',
  'Type RESET to go back to factory defaults.': 'Typ RESET om terug te gaan naar de fabrieksinstellingen.',
  'All settings are removed. The ReToon restarts and shows its WiFi screen; this page will stop answering.': 'Alle instellingen zijn verwijderd. De ReToon herstart en toont zijn WiFi-scherm; deze pagina antwoordt niet meer.',
  'Remove your pages and put back the standard ones?': "Je pagina's verwijderen en de standaardpagina's terugzetten?", 'Done': 'Klaar',
  'ReToon': 'ReToon', 'Home Assistant': 'Home Assistant', 'MQTT': 'MQTT', 'WiFi': 'WiFi', 'WiFi signal': 'WiFi-signaal', 'Boiler module': 'Ketelmodule',
  'Running for': 'Draait al', 'Co-processor': 'Co-processor', 'own build': 'eigen build',
  'The next restart will start the original Toon software, and ReToon will not come back by itself. Continue?': 'De volgende herstart start de originele Toon-software, en ReToon komt niet vanzelf terug. Doorgaan?',
  'Saved. ReToon starts at every restart.': 'Opgeslagen. ReToon start bij elke herstart.', 'Saved. The next restart starts the original Toon software.': 'Opgeslagen. De volgende herstart start de originele Toon-software.',
  'Saved. The web interface restarts.': 'Opgeslagen. De webinterface herstart.',
  'Fetch the new ReToon and put it in place? It is used from the next restart.': 'De nieuwe ReToon ophalen en klaarzetten? Hij wordt gebruikt vanaf de volgende herstart.',
  'Restart the Toon now? ReToon starts again.': 'De Toon nu herstarten? ReToon start daarna weer.',
  'Restart the Toon now? It will start the original Toon software, because "Always start ReToon" is off (System tab).': 'De Toon nu herstarten? Dan start de originele Toon-software, want "Altijd ReToon starten" staat uit (tabblad Systeem).',
  'Restarting… this page works again in about a minute.': 'Herstarten… deze pagina werkt over ongeveer een minuut weer.',
  'OpenTherm over WiFi (ESPHome)': 'OpenTherm via wifi (ESPHome)',
  "Instead of the boiler module and its cable: an ESP32 or ESP8266 with an OpenTherm adapter at the boiler, running ESPHome. The Toon keeps doing the thermostat work and reaches the ESP over WiFi, encrypted. Fill in the board and the adapter's pins, choose Use this ESP, and install the configuration that appears below with ESPHome.":
    'In plaats van de ketelmodule en zijn kabel: een ESP32 of ESP8266 met een OpenTherm-adapter bij de ketel, met ESPHome. De Toon blijft de thermostaat en bereikt de ESP via wifi, versleuteld. Vul het bord en de pinnen van de adapter in, kies Deze ESP gebruiken, en zet de configuratie die hieronder verschijnt met ESPHome op de ESP.',
  'Board': 'Bord', 'Device name': 'Apparaatnaam',
  'Fill in the pins as your adapter\'s description gives them for ESPHome (in_pin and out_pin).':
    'Vul de pinnen in zoals de beschrijving van je adapter ze voor ESPHome geeft (in_pin en out_pin).',
  'Address (empty: the device name.local)': 'Adres (leeg: de apparaatnaam.local)',
  'Use this ESP': 'Deze ESP gebruiken', 'Not tested yet': 'Nog niet getest',
  'New, and not tested with a real boiler or heat pump yet. Try it while you can keep an eye on the heating, and tell us how it works.':
    'Nieuw, en nog niet getest met een echte ketel of warmtepomp. Probeer het als je de verwarming in de gaten kunt houden, en laat weten hoe het werkt.',
  'Installation': 'Installatie', 'Connection status': 'Status aansluiting',
  'Wired: the boiler module': 'Bedraad: de ketelmodule', 'Wireless: OpenTherm over WiFi (ESPHome)': 'Draadloos: OpenTherm via wifi (ESPHome)',
  'External: through Home Assistant': 'Extern: via Home Assistant', 'Also cool with an external device (airco)': 'Ook koelen met een extern apparaat (airco)',
  'Stop using the ESP? It then no longer gets the heating demand.': 'Stoppen met de ESP? Hij krijgt dan de warmtevraag niet meer.',
  "An ESP32 or ESP8266 with an OpenTherm adapter at the boiler or heat pump, running ESPHome, instead of the boiler module and its cable. The Toon keeps doing the thermostat work and reaches the ESP over WiFi, encrypted. Fill in the board and the adapter's pins, choose Use this ESP, and install the configuration that appears below with ESPHome.": "Een ESP32 of ESP8266 met een OpenTherm-adapter bij de ketel of warmtepomp, met ESPHome, in plaats van de ketelmodule en zijn kabel. De Toon blijft de thermostaat en bereikt de ESP via wifi, versleuteld. Vul het bord en de pinnen van de adapter in, kies Deze ESP gebruiken, en zet de configuratie die hieronder verschijnt met ESPHome op de ESP.",
  "Nothing connected to the Toon: \"Heat request\" and \"Cool request\" in Home Assistant (and on the screen) follow the thermostat, and an automation there switches a heater, a heat pump or an airco. The thermostat still decides when to heat or cool. Switch cooling on under Cooling.": "Niets aangesloten op de Toon: \"Warmtevraag\" en \"Koelvraag\" in Home Assistant (en op het scherm) volgen de thermostaat, en een automatisering daar schakelt een verwarming, warmtepomp of airco. De thermostaat beslist nog steeds wanneer er verwarmd of gekoeld wordt. Zet koelen aan onder Koelen.",
  "Next to heating that cannot cool (on/off, or OpenTherm without cooling): \"Cool request\" in Home Assistant follows the thermostat, so an automation can switch an airco; the heating is then just not asked for heat. Switch cooling on under Cooling.": "Naast verwarming die niet kan koelen (aan/uit, of OpenTherm zonder koelen): \"Koelvraag\" in Home Assistant volgt de thermostaat, zodat een automatisering een airco kan schakelen; de verwarming krijgt dan gewoon geen warmtevraag. Zet koelen aan onder Koelen.",
  'Logs': 'Logboeken',
  'The module has a 1-wire temperature sensor': 'De module heeft een 1-wire-temperatuursensor',
  'Pin of the temperature sensor': 'Pin van de temperatuursensor',
  "Some adapters have a small temperature sensor (DS18B20). The module usually sits next to the installation, so it is no room temperature: it comes in Home Assistant as \"OpenTherm module temperature\", and can be chosen there like any other sensor.": "Sommige adapters hebben een kleine temperatuursensor (DS18B20). De module zit meestal naast de installatie, dus het is geen kamertemperatuur: hij komt in Home Assistant als \"OpenTherm module temperature\" en is daar te kiezen zoals elke andere sensor.",
  'The temperature sensor needs a pin of its own': 'De temperatuursensor heeft een eigen pin nodig', 'Back to the boiler module': 'Terug naar de ketelmodule',
  "The configuration for ESPHome: make a new device in ESPHome (the Home Assistant add-on or ESPHome on a computer), replace its configuration by this one and install it, the first time over USB. The WiFi name and password come from ESPHome's secrets (wifi_ssid and wifi_password). It holds this Toon's key for the ESP: keep it to yourself.":
    'De configuratie voor ESPHome: maak in ESPHome (de Home Assistant-add-on of ESPHome op een computer) een nieuw apparaat, vervang de configuratie door deze en installeer hem, de eerste keer via USB. De wifinaam en het wachtwoord komen uit de secrets van ESPHome (wifi_ssid en wifi_password). Er staat de sleutel van deze Toon voor de ESP in: houd hem voor jezelf.',
  'Copy': 'Kopiëren', 'Download': 'Downloaden', 'Copied': 'Gekopieerd',
  'Select and copy it yourself (the browser does not allow it here)': 'Selecteer en kopieer hem zelf (de browser staat het hier niet toe)',
  'Saved: the thermostat now connects to the ESP. Install the configuration below on it.': 'Opgeslagen: de thermostaat maakt nu verbinding met de ESP. Zet de configuratie hieronder erop.',
  'Back to the boiler module on the cable? The ESP then no longer gets the heating demand.': 'Terug naar de ketelmodule aan de kabel? De ESP krijgt dan de warmtevraag niet meer.',
  'ESP not connected': 'ESP niet verbonden', 'Not connected': 'Niet verbonden',
  "Connected, the installation does not answer": "Verbonden, de installatie antwoordt niet",
  'Device': 'Apparaat', 'Configuration': 'Configuratie', 'Older version: install the configuration below again for setting the hot water temperature and for cooling': 'Oudere versie: installeer de configuratie hieronder opnieuw om de warmwatertemperatuur in te stellen en voor koelen', 'Temperature sensor': 'Temperatuursensor', 'Message': 'Melding',
  'OpenTherm module (ESPHome) connection lost': 'Verbinding met de OpenTherm-module (ESPHome) verbroken',
  'an OpenTherm module on an ESP has no on/off contact': 'een OpenTherm-module op een ESP heeft geen aan/uit-contact',
  'New page': 'Nieuwe pagina', 'Unsaved changes': 'Niet-opgeslagen wijzigingen', 'Asking Home Assistant for an access token…': 'Home Assistant om een toegangstoken vragen…',
  'Logged in. This ReToon now uses its own access token.': 'Ingelogd. Deze ReToon gebruikt nu zijn eigen toegangstoken.',
  // texts from the Toon's own programs (toon-update, toon-boilerd, toon-m4d)
  'Looking for an update': 'Zoeken naar een update', 'Looking for the newest version': 'Zoeken naar de nieuwste versie',
  'Putting the new version in place': 'De nieuwe versie klaarzetten', 'Writing the ReToon kernel': 'De ReToon-kernel schrijven', 'Starting the update': 'De update starten',
  'on/off contact selected': 'aan/uit-contact gekozen', 'looking for an OpenTherm device': 'zoekt een OpenTherm-apparaat', 'OpenTherm device found': 'OpenTherm-apparaat gevonden',
  'waiting for the boiler module': 'wacht op de ketelmodule', 'boiler module stopped answering': 'ketelmodule antwoordt niet meer',
  'OpenTherm device answering again': 'OpenTherm-apparaat antwoordt weer',
  'OpenTherm stopped answering, now on/off': 'OpenTherm antwoordt niet meer, nu aan/uit', 'Already busy with an update': 'Al bezig met een update',
  'OpenTherm device not answering, connecting again': 'OpenTherm-apparaat antwoordt niet, opnieuw verbinden',
  'OpenTherm device not answering; OpenTherm stays on, the Toon keeps trying': 'OpenTherm-apparaat antwoordt niet; OpenTherm blijft aan, de Toon blijft het proberen',
  'no OpenTherm device found, back to on/off': 'geen OpenTherm-apparaat gevonden, terug naar aan/uit', 'module did not answer': 'module antwoordde niet',
  "Connection": "Aansluiting", 'Use OpenTherm': 'OpenTherm gebruiken', 'Use the on/off contact': 'Aan/uit-contact gebruiken',
  "Use OpenTherm looks for OpenTherm (about 5 seconds); without it the on/off contact stays in use. Once OpenTherm has worked on this Toon it stays OpenTherm: when the installation stops answering, the Toon keeps the OpenTherm line going and keeps trying, and shows that it does not answer. Only you switch to the on/off contact, here.": "OpenTherm gebruiken zoekt OpenTherm (ongeveer 5 seconden); zonder OpenTherm blijft het aan/uit-contact in gebruik. Als OpenTherm eenmaal op deze Toon heeft gewerkt, blijft het OpenTherm: als de installatie niet meer antwoordt, houdt de Toon de OpenTherm-lijn in stand, blijft hij het proberen en laat hij zien dat er geen antwoord komt. Alleen jij schakelt hier over naar het aan/uit-contact.",
  'Looking for OpenTherm…': 'OpenTherm zoeken…', 'On/off contact': 'Aan/uit-contact', 'Module': 'Module', 'OpenTherm, not answering': 'OpenTherm, antwoordt niet',
  "Looking for OpenTherm…": "OpenTherm zoeken…",
  'Use the on/off contact? The Toon then no longer looks for OpenTherm by itself.': 'Het aan/uit-contact gebruiken? De Toon zoekt dan niet meer zelf naar OpenTherm.',
  // the ReToon's own values (local-entities.json, in the card editor)
  'Room temperature': 'Kamertemperatuur', 'Internal temperature': 'Temperatuur in de Toon', 'Humidity': 'Luchtvochtigheid', 'Light': 'Licht', 'Air quality': 'Luchtkwaliteit', 'Measuring…': 'Aan het meten…', '{0} · measuring…': '{0} · meten…', 'Getting worse': 'Wordt slechter',
  'Getting better': 'Wordt beter', '{0} · learning': '{0} · lerend', 'Good': 'Goed', 'Fair': 'Redelijk', 'Moderate': 'Matig', 'Poor': 'Slecht',
  "Flow temperature": "Aanvoertemperatuur", 'Return water': 'Retourwater', 'Flow setpoint': 'Gevraagde aanvoertemperatuur', 'Hot water setpoint': 'Gevraagde warmwatertemperatuur',
  'Maximum heating setpoint': 'Maximale aanvoertemperatuur', 'Modulation': 'Modulatie', 'Hot water flow': 'Warmwaterdoorstroming', 'Solar storage temperature': 'Temperatuur zonneboiler', 'Solar collector temperature': 'Temperatuur zonnecollector', "Flow temperature circuit 2": "Aanvoertemperatuur circuit 2", 'Hot water temperature 2': 'Warmwatertemperatuur 2', 'Exhaust temperature': 'Rookgastemperatuur', 'Heat exchanger temperature': 'Temperatuur warmtewisselaar', 'Flame current': 'Vlamstroom', 'Heating active': 'Verwarming actief', 'Hot water active': 'Warm water actief', 'Cooling active': 'Koelen actief', 'Remote room setpoint': 'Kamertemperatuur op afstand', "Maximum power": "Maximaal vermogen", 'Minimum modulation': 'Minimale modulatie', 'Hot water temperature': 'Warmwatertemperatuur', 'Fan speed setpoint': 'Ingestelde ventilatorsnelheid', 'Fan speed': 'Ventilatorsnelheid', 'Hot water setpoint upper limit': 'Bovengrens warmwatertemperatuur', 'Hot water setpoint lower limit': 'Ondergrens warmwatertemperatuur', 'Unsuccessful burner starts': 'Mislukte branderstarts', 'Flame signal too low': 'Vlamsignaal te laag', 'OEM diagnostic code': 'Diagnosecode fabrikant', 'Burner starts': 'Branderstarts', 'Heating pump starts': 'Starts pomp verwarming', 'Hot water pump starts': 'Starts pomp warm water', 'Hot water burner starts': 'Branderstarts warm water', 'Burner hours': 'Branderuren', 'Heating pump hours': 'Draaiuren pomp verwarming', 'Hot water pump hours': 'Draaiuren pomp warm water', 'Hot water burner hours': 'Branderuren warm water', "OpenTherm version (installation)": "OpenTherm-versie (installatie)", "Installation product type": "Producttype installatie", "Installation product version": "Productversie installatie", 'Water pressure': 'Waterdruk', 'OpenTherm module temperature': 'Temperatuur OpenTherm-module', 'Fault code': 'Foutcode', 'Burner': 'Brander',
  'Heat request': 'Warmtevraag', 'Cool request': 'Koelvraag', ', with an external device': ', met een extern apparaat', 'Heat and cool with an external device': 'Verwarmen en koelen met een extern apparaat', '"Heat request" and "Cool request" (in Home Assistant and on the screen) follow the thermostat, so an automation can switch a heater or an airco. Without a boiler module the thermostat then still decides when to heat or cool. Next to a boiler that can not cool (on/off, or OpenTherm without cooling), cooling goes to the external device only: the boiler is then just not asked for heat. Switch cooling on under Cooling.': '"Warmtevraag" en "Koelvraag" (in Home Assistant en op het scherm) volgen de thermostaat, zodat een automatisering een kachel of airco kan schakelen. Zonder ketelmodule beslist de thermostaat dan toch wanneer er verwarmd of gekoeld moet worden. Naast een ketel die niet kan koelen (aan/uit, of OpenTherm zonder koelen) gaat het koelen alleen naar het externe apparaat: de ketel krijgt dan gewoon geen warmtevraag. Zet koelen aan onder Koelen.', "Installation fault": "Storing installatie", 'Display': 'Scherm',

  // texts with values: {0}, {1}... in the order of the ${} in app.js
  'Page {0}': 'Pagina {0}', 'Remove the page "{0}"?': 'De pagina "{0}" verwijderen?', 'Cards on "{0}"': 'Kaarten op "{0}"', 'Group {0}': 'Groep {0}',
  'The ReToon cannot show a "{0}" card. You can keep it, change it here, or remove it.': 'De ReToon kan een kaart "{0}" niet tonen. Je kunt hem houden, hier aanpassen of verwijderen.',
  ' — {0} cards': ' — {0} kaarten', '; shown as a plain tile or list: {0}': '; getoond als gewone tegel of lijst: {0}', '; cannot be shown: {0}': '; niet te tonen: {0}',
  'long-lived access token "Toon {0}"': 'langlevend toegangstoken "Toon {0}"', ', made by logging in on {0}': ', gemaakt door in te loggen op {0}',
  'Certificate fingerprint (SHA-256): {0}': 'Vingerafdruk van het certificaat (SHA-256): {0}',
  'yes, {0} advertisements in the last minute': 'ja, {0} advertenties in de laatste minuut', 'Volume {0} %': 'Volume {0} %',
  '{0}, channel {1}': '{0}, kanaal {1}', '{0} attached to this ReToon, ': '{0} aangesloten op deze ReToon, ', '{0} other routers': '{0} andere routers',
  'Saved. The ReToon is now at https://{0}.local/ (or its address); open it there.': 'Opgeslagen. De ReToon staat nu op https://{0}.local/ (of zijn adres); open hem daar.',
  ' (average of {0} of {1} sensors)': ' (gemiddelde van {0} van {1} sensoren)', ', warming up to {0} °C': ', opwarmen naar {0} °C',
  'on hold until {0}': 'vastgehouden tot {0}', 'next: {0}': 'volgende: {0}', '{0} at {1}': '{0} om {1}',
  "Learned so far: the house warms up about {0} °C an hour with the heating at full temperature": "Tot nu toe geleerd: het huis warmt ongeveer {0} °C per uur op met de verwarming op volle temperatuur",
  "Not learned yet how fast the house warms up ({0} of 3 measurements); assuming {1} °C an hour with the heating at full temperature": "Nog niet geleerd hoe snel het huis opwarmt ({0} van 3 metingen); aangenomen {1} °C per uur met de verwarming op volle temperatuur",
  ' and loses about {0} °C an hour without heating.': ' en verliest ongeveer {0} °C per uur zonder verwarming.', ' Flow temperature asked for now: {0} °C.': ' Nu gevraagde aanvoertemperatuur: {0} °C.',
  'Install ReToon {0}': 'ReToon {0} installeren', 'Remove {0} and its settings?': '{0} en de instellingen ervan verwijderen?',
  '{0} is installed': '{0} is geïnstalleerd', '{0} is updated': '{0} is bijgewerkt', '{0} is removed': '{0} is verwijderd', 'Update to {0}': 'Bijwerken naar {0}',
  '{0}: not a number': '{0}: geen getal', 'By {0}': 'Door {0}', 'May use in Home Assistant: {0}': 'Mag in Home Assistant gebruiken: {0}', 'May show cameras: {0}': 'Mag camera\'s tonen: {0}', 'all': 'alle', '{0} ({1} dBm) · {2}': '{0} ({1} dBm) · {2}',
  '{0} advertisements a minute': '{0} advertenties per minuut', '{0} devices attached': '{0} apparaten aangesloten', 'Room {0} °C · {1}': 'Kamer {0} °C · {1}',
  'ReToon {0} is installed; it is used from the next restart.': 'ReToon {0} is geïnstalleerd; hij wordt gebruikt vanaf de volgende herstart.',
  'ReToon {0} is available. You have {1}.': 'ReToon {0} is beschikbaar. Je hebt {1}.', '{0} °C ({1})': '{0} °C ({1})', ', next {0}': ', daarna {0}',
  '{0} h {1} min': '{0} u {1} min', ' · {0} % humidity': ' · {0} % luchtvochtigheid',
  'Keep a log of thermostat changes': 'Wijzigingen van de thermostaat bijhouden',
  'A line for each change of the target, preset, hold or week program, with where it came from: the screen, the web interface, Home Assistant, an app, or the week program itself. Kept on the Toon, also over a restart. Off by default.': 'Een regel voor elke wijziging van de doeltemperatuur, stand, tijdelijke of vaste keuze of het weekprogramma, met waar die vandaan kwam: het scherm, de webinterface, Home Assistant, een app of het weekprogramma zelf. Bewaard op de Toon, ook na een herstart. Standaard uit.',
  'Show the log': 'Log tonen', 'Nothing logged yet.': 'Nog niets bijgehouden.', 'The log is off.': 'Het bijhouden staat uit.',
};

/* texts the Toon's programs make with values in them */
const NL_PATTERNS = [
  [/^OpenTherm module \(ESPHome\) connected: (.*)$/, 'OpenTherm-module (ESPHome) verbonden: $1'],
  [/^OpenTherm module \(ESPHome\) not reachable: (.*)$/, 'OpenTherm-module (ESPHome) niet bereikbaar: $1'],
  [/^The Toon cannot reach Home Assistant at (.+)$/, 'De Toon kan Home Assistant niet bereiken op $1'],
  [/^Connected, (\d+) values found$/, 'Verbonden, $1 waarden gevonden'],
  [/^(\d+) values found$/, '$1 waarden gevonden'],
  [/^Own broker, (\d+) values found$/, 'Eigen broker, $1 waarden gevonden'],
  [/^Running \(firmware ([\d.]+)\)(.*)$/, 'Actief (firmware $1)$2'],
  [/^Not running, next try in (\d+) s \((.*)\)$/, 'Draait niet, volgende poging over $1 s ($2)'],
  [/^ReToon is up to date \(version (.*)\)\.$/, 'ReToon is bijgewerkt (versie $1).'],
  [/^ReToon (.*) is available\.$/, 'ReToon $1 is beschikbaar.'],
  [/^Fetching ReToon (.*)$/, 'ReToon $1 ophalen'],
  [/^Checking (.*)$/, '$1 controleren'],
  [/^ReToon (.*) is in place\. Restart to start it\.$/, 'ReToon $1 staat klaar. Herstart om hem te starten.'],
  [/^(.*) \(ReToon\)$/, (m, n) => `${NL[n] || n} (ReToon)`],
];

function tr(s) {
  if (LANG !== 'nl' || !s) return s;
  if (Object.prototype.hasOwnProperty.call(NL, s)) return NL[s];
  for (const [re, to] of NL_PATTERNS) if (re.test(s)) return s.replace(re, to);
  return s;
}

/* T('text') or T`text with ${values}` */
function T(s, ...vals) {
  if (Array.isArray(s) && s.raw) {
    const key = s.reduce((a, part, i) => a + (i ? `{${i - 1}}` : '') + part, '');
    const t = LANG === 'nl' && Object.prototype.hasOwnProperty.call(NL, key) ? NL[key] : key;
    return t.replace(/\{(\d+)\}/g, (m, i) => vals[i]);
  }
  return tr(s);
}

/* translate a text node or the placeholder/title of an element, keeping the
   spaces around the text (they matter between inline elements) */
function trNode(n) {
  if (LANG !== 'nl') return;
  const host = n.nodeType === 3 ? n.parentElement : n;
  if (!host || host.closest('[translate="no"], script, style, code, pre, textarea')) return;
  if (n.nodeType === 3) {
    const v = n.nodeValue, key = v.replace(/\s+/g, ' ').trim();
    if (!key || !/[A-Za-z]/.test(key)) return;
    const t = tr(key);
    if (t !== key) n.nodeValue = v.match(/^\s*/)[0] + t + v.match(/\s*$/)[0];
    return;
  }
  for (const a of ['placeholder', 'title']) {
    const v = n.getAttribute && n.getAttribute(a);
    if (v) { const t = tr(v); if (t !== v) n.setAttribute(a, t); }
  }
}
function trTree(root) {
  if (root.nodeType === 3) return trNode(root);
  if (root.nodeType !== 1) return;
  trNode(root);
  const w = document.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT);
  for (let n = w.nextNode(); n; n = w.nextNode()) trNode(n);
}
if (LANG === 'nl') {
  document.addEventListener('DOMContentLoaded', () => trTree(document.body));
  new MutationObserver(list => {
    for (const m of list) {
      if (m.type === 'childList') m.addedNodes.forEach(trTree);
      else if (m.type === 'characterData') trNode(m.target);
      else trNode(m.target);
    }
  }).observe(document.documentElement, { childList: true, subtree: true, characterData: true,
    attributes: true, attributeFilter: ['placeholder', 'title'] });
}
