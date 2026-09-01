# Antenna-Switch-ESP32
Système de gestion pour commutateur d'antennes

<img width="617" height="250" alt="image" src="https://github.com/user-attachments/assets/44e21be0-e699-4d9a-b0b9-85c426d76eea" />
Voici l'architecture générale du projet : deux cartes ESP32 qui communiquent en I2C, l'une gérant l'écran tactile, l'autre pilotant les relais et le serveur web.

Le second point intéressant, c'est la logique de l'écran tactile : un appui court bascule un relais, un appui long (700 ms) ouvre un clavier pour le renommer.

<img width="723" height="399" alt="image" src="https://github.com/user-attachments/assets/1dd408ec-7074-4743-a1a7-62286477c4c4" />

A retenir sur les points d'attention signalés dans le README :

Le brochage exact de l'écran et du tactile dépend de la révision de la carte AliExpress — non garanti sans photo.
La synchronisation des noms n'est complète que dans un sens (écran → WT32 → page web) ; un renommage fait depuis la page web n'est pas relu automatiquement par l'écran.
Le bus I2C doit être en 3,3 V avec une masse commune entre les deux cartes.
