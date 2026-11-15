# Composants tiers

- FFmpeg : décodeur vidéo requis à la compilation, non inclus en binaire ni en source dans ce dépôt. Sa licence peut être LGPL ou GPL selon les options utilisées ; voir https://ffmpeg.org/legal.html. Toute distribution d'un binaire lié doit conserver la configuration et les sources correspondantes.
- FourthTube : certaines adaptations du lecteur et du support des threads proviennent de cette base ; voir https://github.com/erievs/FourthTube et ses notices.
- `makerom` et `bannertool` : outils de paquetage à obtenir séparément.

La GPL du code SHIELD ne remplace pas les licences des composants tiers.

## Vérification des archives locales FFmpeg

Le dossier de travail local de ce projet contient les mêmes six archives FFmpeg, octet pour octet, que ceux de MP3 Downloader et de l’autre projet MP4. Les empreintes et la version source revendiquée par le fichier de build hérité figurent dans [l’audit MP3 Downloader](https://github.com/Shield-77/Shield-MP3-downloader/blob/main/LICENSING_STATUS.md). Cette égalité des archives **ne démontre pas** leur présence exacte dans les CIA de release ni la correspondance du commit source indiqué. Il faut conserver les journaux de compilation et produire les sources, modifications et options réellement utilisées avant de certifier la conformité des binaires.
