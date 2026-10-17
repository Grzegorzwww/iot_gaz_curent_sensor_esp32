1) Co ma być dodane w firmware ESP
ESP ma publikować zużycie energii w nowym formacie:

temat bazowy: <token>/zurzycie_pradu
przykłady:
<token>/zurzycie_pradu
<token>/zurzycie_pradu/kwh
<token>/zurzycie_pradu/power_w
opcjonalnie też inne metryki, ale portal zapisuje tylko kwh i power_w jako główne
<token> to losowy, nieczytelny identyfikator, nie e-mail
temat bez /kwh ma być traktowany jako kwh
payload to liczba (float), np. 12.345, 250.5
2) Ważne ograniczenia
Nie używamy wesola88/... ani żadnych starych tematów
Nie zmieniamy obecnego dashboardu i nie ruszamy app / web
Portal czyta tylko z MQTT; nie wysyła komend do ESP
Nie ma żądania “kontrola urządzenia” z portalu; to jest read-only
3) Model użytkownika
użytkownik ma konto w portalu api.warcgr.pl
jedno konto może mieć wiele ESP
każdy ESP ma swój własny token
ten sam użytkownik może dodawać kilka urządzeń, każdemu przydzielony jest osobny token
4) Broker / bezpieczeństwo
obecnie broker ma wspólne dane logowania dla wszystkich
nie zakładamy per-device ACL ani per-token auth
token jest kluczem rozdzielającym dane użytkownika
nie publikujemy e-maila w topicach, bo to PII i ma problem przy zmianie maila
5) Co musi być “dobrze zrobione” w embeded
payload musi być parse’owalny jako float
ignorować puste / błędne / nan / inf / nieprawidłowe wartości
nie wysyłać nic do starych tematów
jeśli ESP ma zliczanie impulsów, kwh powinien być liczbą energii, a nie surowym impulsem
power_w to wartość chwilowa, np. moc chwilowa w watach
Gotowy prompt do agenta embedded
“W repo istnieje osobny portal multi-user dla api.warcgr.pl, który nie rusza starego iot.warcgr.pl. Dla ESP trzeba dodać nowy namespace MQTT:

temat: <token>/zurzycie_pradu
warianty: <token>/zurzycie_pradu/kwh, <token>/zurzycie_pradu/power_w
token = losowy 32+ znakowy identyfikator, nie e-mail
payload = float / liczba
token/zurzycie_pradu traktować jako kwh
nie publikować do wesola88/..., piec_gazowy/... ani innych starych tematów
ignorować błędne wartości
portal jest read-only, nie wysyła komend do ESP
istniejący system ma pozostać bez zmian”