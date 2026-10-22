#!/usr/bin/env bash
# Build High Cube locally and install or update it on another host.
# Usage: HC_DEST_USER=<user> ./install.sh <destination-ip> \
#          [--role admin|agent|both] [--db true] [--mode install|update]
#
# The script works for both a fresh install and an update of an existing host.
# When run from a terminal it shows two menus: the action (install when the host
# is empty, update when High Cube is already present) and the role to install
# (admin, agent, or both). Pass --mode and/or --role to skip the menus, which is
# also the default when the script runs without a terminal.
#
# Roles:
#   both  (default) control panel + a co-located worker on the same host.
#   admin           control panel only (hc-admin, NGINX, panel).
#   agent           worker only (hc-agent). Requires HC_ADMIN_URL and
#                   HC_AGENT_TOKEN so it can reach an existing control panel.
#
# On a fresh install the destination is probed first (preflight) to confirm it
# has everything the chosen role needs.
#
# Each stage is checked. The script exits on the first failure.
# Installing and reloading NGINX needs sudo on the destination. If sudo is not
# passwordless, run this script from a terminal so the password prompt works.
#
# HC_NGINX_DEFER=1 installs and configures NGINX but leaves it stopped and
# disabled, for hosts where another proxy still owns ports 80 and 443. Start it
# later with: sudo systemctl enable --now nginx
set -euo pipefail

# ASCII rendering of logo.png (generated). Green truecolor with a
# shaded-block fallback when the terminal has no color.
LOGO_LINES=(
  '                          \033[38;2;0;0;0m░\033[38;2;13;11;15m░\033[38;2;25;19;17m░\033[38;2;11;11;15m░\033[38;2;0;0;0m░\033[0m'
  '                     \033[38;2;0;0;2m░\033[38;2;23;21;16m░\033[38;2;27;17;15m░\033[38;2;61;73;24m░\033[38;2;101;124;31m▒\033[38;2;90;114;28m▒\033[38;2;145;209;32m▓\033[38;2;158;230;33m▓\033[38;2;141;205;33m▓\033[38;2;86;108;28m▒\033[38;2;99;121;32m▒\033[38;2;44;52;20m░\033[38;2;32;19;19m░\033[38;2;8;6;14m░\033[38;2;0;0;0m░\033[0m'
  '                 \033[38;2;0;0;6m░\033[38;2;47;49;22m░\033[38;2;40;42;19m░\033[38;2;100;130;30m▒\033[38;2;111;146;32m▒\033[38;2;143;211;35m▓\033[38;2;155;232;36m▓\033[38;2;150;254;30m▓\033[38;2;142;255;26m▓\033[38;2;121;242;33m▓\033[38;2;89;221;27m▓\033[38;2;81;220;24m▓\033[38;2;93;227;26m▓\033[38;2;142;255;29m▓\033[38;2;144;255;25m▓\033[38;2;151;246;31m▓\033[38;2;157;231;37m▓\033[38;2;125;183;33m▓\033[38;2;100;124;29m▒\033[38;2;79;101;26m▒\033[38;2;45;42;20m░\033[38;2;28;30;17m░\033[38;2;0;0;2m░\033[0m'
  '            \033[38;2;2;0;0m░\033[38;2;31;29;14m░\033[38;2;83;86;29m░\033[38;2;59;68;22m░\033[38;2;117;161;33m▒\033[38;2;132;185;35m▓\033[38;2;149;245;35m▓\033[38;2;151;255;35m▓\033[38;2;135;255;29m▓\033[38;2;116;250;31m▓\033[38;2;103;247;28m▓\033[38;2;69;202;29m▓\033[38;2;54;171;31m▒\033[38;2;22;117;34m░\033[38;2;0;64;30m░\033[38;2;0;41;27m░\033[38;2;0;42;28m░\033[38;2;0;62;33m░\033[38;2;15;96;32m░\033[38;2;40;144;31m▒\033[38;2;62;183;31m▒\033[38;2;81;217;26m▓\033[38;2;112;250;28m▓\033[38;2;130;255;28m▓\033[38;2;144;255;28m▓\033[38;2;153;253;35m▓\033[38;2;139;221;35m▓\033[38;2;129;170;36m▓\033[38;2;104;142;32m▒\033[38;2;58;62;22m░\033[38;2;72;77;26m░\033[38;2;11;5;6m░\033[0m'
  '       \033[38;2;0;0;0m░░\033[38;2;0;29;19m░\033[38;2;1;67;26m░\033[38;2;43;111;34m░\033[38;2;98;176;47m▒\033[38;2;119;210;44m▓\033[38;2;149;255;42m▓\033[38;2;144;255;43m▓\033[38;2;117;255;35m▓\033[38;2;83;222;34m▓\033[38;2;66;202;30m▒\033[38;2;24;132;27m▒\033[38;2;13;97;33m░\033[38;2;0;47;28m░\033[38;2;0;45;27m░\033[38;2;0;34;26m░\033[38;2;0;16;18m░\033[38;2;0;12;13m░\033[38;2;3;6;8m░\033[38;2;3;4;5m░\033[38;2;2;2;5m░\033[38;2;3;7;8m░\033[38;2;0;4;10m░\033[38;2;0;17;16m░\033[38;2;0;17;19m░\033[38;2;0;42;26m░\033[38;2;0;45;27m░\033[38;2;7;86;31m░\033[38;2;22;115;32m░\033[38;2;44;171;27m▒\033[38;2;75;210;31m▓\033[38;2;98;241;35m▓\033[38;2;129;255;38m▓\033[38;2;147;255;44m▓\033[38;2;148;255;46m█\033[38;2;97;184;44m▒\033[38;2;85;172;46m▒\033[38;2;14;74;28m░\033[38;2;0;50;28m░\033[38;2;0;0;6m░\033[38;2;0;0;0m░\033[0m'
  '     \033[38;2;1;7;7m░\033[38;2;35;39;23m░\033[38;2;72;77;36m░\033[38;2;117;130;40m▒\033[38;2;51;93;18m░\033[38;2;15;64;18m░\033[38;2;0;76;27m░\033[38;2;0;101;43m░\033[38;2;3;153;46m▒\033[38;2;18;163;42m▒\033[38;2;19;103;29m░\033[38;2;31;97;30m░\033[38;2;28;60;28m░\033[38;2;66;96;32m░\033[38;2;83;108;33m▒\033[38;2;114;162;35m▒\033[38;2;83;108;27m▒\033[38;2;69;73;23m░\033[38;2;37;41;17m░\033[38;2;7;3;9m░\033[38;2;0;0;8m░\033[38;2;0;10;12m░\033[38;2;0;37;21m░\033[38;2;0;48;26m░\033[38;2;0;20;15m░\033[38;2;0;5;10m░\033[38;2;0;0;7m░\033[38;2;8;7;10m░\033[38;2;31;30;18m░\033[38;2;73;81;27m░\033[38;2;99;136;33m▒\033[38;2;105;148;34m▒\033[38;2;86;113;34m▒\033[38;2;51;77;28m░\033[38;2;37;87;31m░\033[38;2;28;100;28m░\033[38;2;16;126;37m░\033[38;2;11;170;47m▒\033[38;2;0;113;44m░\033[38;2;0;74;32m░\033[38;2;8;61;22m░\033[38;2;45;98;21m░\033[38;2;107;124;33m▒\033[38;2;106;111;39m▒\033[38;2;54;57;23m░\033[38;2;4;10;8m░\033[0m'
  '     \033[38;2;0;0;2m░\033[38;2;136;151;51m▒\033[38;2;239;255;79m█\033[38;2;194;255;56m█\033[38;2;206;255;56m█\033[38;2;181;219;42m▓\033[38;2;142;169;28m▓\033[38;2;92;116;17m▒\033[38;2;57;81;18m░\033[38;2;10;29;14m░\033[38;2;0;38;23m░\033[38;2;29;84;35m░\033[38;2;84;133;45m▒\033[38;2;108;203;35m▓\033[38;2;119;231;30m▓\033[38;2;129;255;19m▓\033[38;2;142;255;22m▓\033[38;2;152;255;22m▓\033[38;2;146;245;26m▓\033[38;2;138;205;33m▓\033[38;2;106;143;33m▒\033[38;2;69;100;31m░\033[38;2;29;57;28m░\033[38;2;10;37;24m░\033[38;2;48;76;31m░\033[38;2;84;111;33m▒\033[38;2;129;171;37m▓\033[38;2;138;203;36m▓\033[38;2;156;249;32m▓\033[38;2;155;255;28m█\033[38;2;137;255;21m▓\033[38;2;119;248;24m▓\033[38;2;109;215;33m▓\033[38;2;84;159;39m▒\033[38;2;55;94;41m░\033[38;2;0;19;18m░\033[38;2;1;20;12m░\033[38;2;40;62;16m░\033[38;2;90;110;20m▒\033[38;2;140;154;27m▒\033[38;2;176;209;37m▓\033[38;2;195;245;51m█\033[38;2;177;255;60m█\033[38;2;132;235;67m▓\033[38;2;54;147;56m▒\033[38;2;3;12;10m░\033[0m'
  '     \033[38;2;1;2;3m░\033[38;2;108;120;36m▒\033[38;2;181;238;43m▓\033[38;2;118;241;8m▓\033[38;2;132;246;20m▓\033[38;2;146;255;28m▓\033[38;2;193;255;53m█\033[38;2;180;255;61m█\033[38;2;71;238;37m▓\033[38;2;21;142;36m▒\033[38;2;3;20;12m░\033[38;2;0;25;16m░\033[38;2;0;20;16m░\033[38;2;0;40;25m░\033[38;2;0;58;28m░\033[38;2;2;68;25m░\033[38;2;20;110;28m░\033[38;2;47;148;28m▒\033[38;2;75;197;23m▒\033[38;2;104;236;20m▓\033[38;2;133;255;20m▓\033[38;2;145;255;22m▓\033[38;2;146;234;29m▓\033[38;2;140;206;33m▓\033[38;2;149;255;25m▓\033[38;2;146;255;24m▓\033[38;2;128;255;19m▓\033[38;2;105;237;22m▓\033[38;2;64;183;24m▒\033[38;2;36;134;28m▒\033[38;2;5;80;29m░\033[38;2;0;60;27m░\033[38;2;0;45;23m░\033[38;2;35;69;18m░\033[38;2;87;118;20m▒\033[38;2;146;185;34m▓\033[38;2;175;214;41m▓\033[38;2;182;243;50m█\033[38;2;157;247;55m▓\033[38;2;102;238;49m▓\033[38;2;65;224;45m▓\033[38;2;21;209;36m▒\033[38;2;2;208;27m▒\033[38;2;0;190;33m▒\033[38;2;0;120;51m░\033[38;2;1;11;9m░\033[0m'
  '      \033[38;2;0;0;2m░\033[38;2;122;136;42m▒\033[38;2;186;255;42m█\033[38;2;112;243;5m▓\033[38;2;116;245;7m▓\033[38;2;134;245;12m▓\033[38;2;135;245;54m▓\033[38;2;3;194;45m▒\033[38;2;0;120;49m░\033[38;2;2;13;9m░\033[38;2;2;8;9m░\033[38;2;3;10;10m░\033[38;2;4;11;10m░\033[38;2;15;19;18m░\033[38;2;76;84;38m░\033[38;2;56;81;29m░\033[38;2;6;32;15m░\033[38;2;0;27;23m░\033[38;2;0;45;30m░\033[38;2;8;81;30m░\033[38;2;37;141;28m▒\033[38;2;87;220;22m▓\033[38;2;100;241;21m▓\033[38;2;63;188;24m▒\033[38;2;36;138;27m▒\033[38;2;7;72;29m░\033[38;2;0;44;26m░\033[38;2;3;36;21m░\033[38;2;38;68;19m░\033[38;2;91;119;21m▒\033[38;2;155;183;32m▓\033[38;2;179;221;40m▓\033[38;2;179;242;51m█\033[38;2;153;249;54m▓\033[38;2;105;235;48m▓\033[38;2;61;223;43m▓\033[38;2;13;205;32m▒\033[38;2;0;196;26m▒\033[38;2;0;192;25m▒\033[38;2;0;196;25m▒\033[38;2;0;187;30m▒\033[38;2;4;185;39m▒\033[38;2;6;170;53m▒\033[38;2;3;116;50m░\033[38;2;1;11;9m░\033[0m'
  '      \033[38;2;0;0;0m░\033[38;2;126;136;47m▒\033[38;2;192;255;46m█\033[38;2;114;243;5m▓\033[38;2;122;247;13m▓\033[38;2;139;247;17m▓\033[38;2;138;253;46m▓\033[38;2;18;206;45m▒\033[38;2;1;116;49m░\033[38;2;1;13;10m░\033[38;2;4;15;11m░\033[38;2;3;11;9m░\033[38;2;22;32;16m░\033[38;2;201;221;75m▓\033[38;2;207;255;65m█\033[38;2;205;255;56m█\033[38;2;188;230;47m▓\033[38;2;151;184;34m▓\033[38;2;91;121;18m▒\033[38;2;53;72;15m░\033[38;2;21;38;19m░\033[38;2;3;49;27m░\033[38;2;6;65;31m░\033[38;2;6;35;27m░\033[38;2;33;56;26m░\033[38;2;78;107;15m▒\033[38;2;146;175;28m▓\033[38;2;190;224;39m▓\033[38;2;198;250;50m█\033[38;2;187;255;60m█\033[38;2;121;241;56m▓\033[38;2;46;215;40m▓\033[38;2;7;202;32m▒\033[38;2;0;193;28m▒\033[38;2;0;190;28m▒\033[38;2;0;185;34m▒\033[38;2;1;188;42m▒\033[38;2;5;181;49m▒\033[38;2;5;169;55m▒\033[38;2;4;160;56m▒\033[38;2;3;167;62m▒\033[38;2;2;143;55m▒\033[38;2;1;33;21m░\033[38;2;1;17;12m░\033[38;2;0;9;9m░\033[0m'
  '      \033[38;2;0;0;4m░\033[38;2;110;136;30m▒\033[38;2;157;255;33m█\033[38;2;96;241;13m▓\033[38;2;122;246;12m▓\033[38;2;142;253;15m▓\033[38;2;126;214;55m▓\033[38;2;0;130;49m░\033[38;2;2;98;43m░\033[38;2;1;14;8m░\033[38;2;0;0;8m░░\033[38;2;15;21;14m░\033[38;2;207;237;62m█\033[38;2;139;249;18m▓\033[38;2;123;245;16m▓\033[38;2;152;253;35m▓\033[38;2;176;255;48m█\033[38;2;213;255;58m█\033[38;2;161;241;47m▓\033[38;2;67;182;44m▒\033[38;2;8;31;16m░\033[38;2;0;0;5m░\033[38;2;113;144;30m▒\033[38;2;201;255;42m█\033[38;2;210;255;56m█\033[38;2;150;245;62m▓\033[38;2;77;220;50m▓\033[38;2;33;205;42m▒\033[38;2;3;199;33m▒\033[38;2;0;187;28m▒\033[38;2;0;189;33m▒\033[38;2;1;189;40m▒\033[38;2;5;178;48m▒\033[38;2;5;158;52m▒\033[38;2;4;140;49m▒\033[38;2;2;97;37m░\033[38;2;3;147;47m▒\033[38;2;2;166;52m▒\033[38;2;1;105;39m░\033[38;2;2;74;27m░\033[38;2;1;31;13m░\033[38;2;2;0;2m░\033[0m'
  '      \033[38;2;0;0;6m░\033[38;2;93;135;11m▒\033[38;2;129;255;22m▓\033[38;2;64;240;28m▓\033[38;2;91;244;21m▓\033[38;2;115;251;13m▓\033[38;2;132;218;41m▓\033[38;2;130;184;61m▓\033[38;2;169;226;49m▓\033[38;2;165;211;34m▓\033[38;2;125;166;23m▒\033[38;2;74;104;14m░\033[38;2;27;43;8m░\033[38;2;54;64;22m░\033[38;2;195;245;55m█\033[38;2;124;248;12m▓\033[38;2;116;245;7m▓\033[38;2;104;243;2m▓\033[38;2;168;249;36m█\033[38;2;81;200;75m▓\033[38;2;0;174;64m▒\033[38;2;6;42;19m░\033[38;2;0;0;6m░\033[38;2;103;167;13m▒\033[38;2;178;255;23m█\033[38;2;198;238;78m█\033[38;2;4;192;34m▒\033[38;2;0;191;27m▒\033[38;2;0;186;33m▒\033[38;2;1;158;40m▒\033[38;2;5;162;51m▒\033[38;2;4;142;46m▒\033[38;2;3;100;34m░\033[38;2;2;63;25m░\033[38;2;1;30;14m░\033[38;2;1;7;6m░\033[38;2;1;0;3m░\033[38;2;1;11;10m░\033[38;2;1;19;12m░\033[38;2;1;0;2m░\033[0m'
  '      \033[38;2;0;0;6m░\033[38;2;101;136;13m▒\033[38;2;109;255;34m▓\033[38;2;20;231;47m▓\033[38;2;47;240;40m▓\033[38;2;58;242;34m▓\033[38;2;87;250;22m▓\033[38;2;121;255;24m▓\033[38;2;146;252;33m▓\033[38;2;169;255;46m█\033[38;2;192;255;52m█\033[38;2;205;255;55m█\033[38;2;195;250;46m█\033[38;2;149;180;38m▓\033[38;2;202;235;63m█\033[38;2;132;251;15m▓\033[38;2;125;246;13m▓\033[38;2;113;246;9m▓\033[38;2;169;253;33m█\033[38;2;89;203;70m▓\033[38;2;2;171;61m▒\033[38;2;7;39;18m░\033[38;2;0;0;6m░\033[38;2;104;161;15m▒\033[38;2;176;255;23m█\033[38;2;196;242;69m█\033[38;2;19;199;36m▒\033[38;2;3;204;33m▒\033[38;2;4;147;42m▒\033[38;2;3;158;56m▒\033[38;2;2;126;36m░\033[38;2;1;10;4m░\033[38;2;1;0;5m░\033[38;2;1;0;6m░\033[38;2;1;11;11m░\033[38;2;1;30;18m░\033[38;2;2;45;24m░\033[38;2;5;100;45m░\033[38;2;4;52;26m░\033[0m'
  '      \033[38;2;1;1;7m░\033[38;2;104;138;13m▒\033[38;2;102;255;37m▓\033[38;2;0;220;57m▒\033[38;2;11;227;54m▓\033[38;2;15;228;52m▓\033[38;2;22;238;50m▓\033[38;2;41;252;42m▓\033[38;2;56;248;30m▓\033[38;2;70;243;20m▓\033[38;2;90;241;19m▓\033[38;2;112;242;25m▓\033[38;2;137;249;34m▓\033[38;2;168;250;50m█\033[38;2;183;247;53m█\033[38;2;114;247;13m▓\033[38;2;118;247;13m▓\033[38;2;116;247;8m▓\033[38;2;173;253;35m█\033[38;2;88;201;73m▓\033[38;2;0;171;63m▒\033[38;2;6;40;19m░\033[38;2;0;0;6m░\033[38;2;87;163;15m▒\033[38;2;168;255;22m█\033[38;2;201;242;75m█\033[38;2;17;201;37m▒\033[38;2;0;205;33m▒\033[38;2;3;143;41m▒\033[38;2;2;180;59m▒\033[38;2;2;106;29m░\033[38;2;1;0;0m░\033[38;2;1;21;14m░\033[38;2;2;60;30m░\033[38;2;3;69;34m░\033[38;2;4;69;34m░\033[38;2;1;57;30m░\033[38;2;0;76;42m░\033[38;2;0;26;20m░\033[38;2;11;17;7m░\033[38;2;18;36;8m░\033[38;2;0;0;0m░\033[0m'
  '      \033[38;2;0;0;6m░\033[38;2;104;133;12m▒\033[38;2;106;255;41m▓\033[38;2;0;211;62m▒\033[38;2;7;216;57m▒\033[38;2;14;228;59m▓\033[38;2;47;182;38m▒\033[38;2;3;132;33m░\033[38;2;10;194;46m▒\033[38;2;17;230;50m▓\033[38;2;32;247;48m▓\033[38;2;44;249;39m▓\033[38;2;55;245;28m▓\033[38;2;67;244;20m▓\033[38;2;87;247;15m▓\033[38;2;88;246;15m▓\033[38;2;98;248;14m▓\033[38;2;97;245;10m▓\033[38;2;158;253;32m█\033[38;2;86;197;68m▓\033[38;2;0;151;63m▒\033[38;2;5;35;18m░\033[38;2;0;0;5m░\033[38;2;42;161;24m▒\033[38;2;107;255;30m▓\033[38;2;177;241;48m█\033[38;2;15;195;37m▒\033[38;2;0;167;55m▒\033[38;2;3;121;53m░\033[38;2;2;145;56m▒\033[38;2;3;105;33m░\033[38;2;4;4;1m░\033[38;2;3;15;11m░\033[38;2;0;22;15m░\033[38;2;0;28;20m░\033[38;2;0;33;23m░\033[38;2;10;59;25m░\033[38;2;86;128;25m▒\033[38;2;132;170;23m▒\033[38;2;177;222;32m▓\033[38;2;192;229;39m▓\033[38;2;140;167;38m▓\033[38;2;72;101;28m░\033[38;2;35;58;16m░\033[38;2;3;10;8m░\033[0m'
  '     \033[38;2;1;2;7m░\033[38;2;77;105;15m▒\033[38;2;168;225;22m▓\033[38;2;76;205;42m▓\033[38;2;1;181;61m▒\033[38;2;2;206;63m▒\033[38;2;18;223;61m▓\033[38;2;93;190;35m▓\033[38;2;0;66;36m░\033[38;2;2;37;20m░\033[38;2;0;51;17m░\033[38;2;1;107;31m░\033[38;2;4;168;45m▒\033[38;2;11;216;53m▒\033[38;2;20;240;53m▓\033[38;2;24;238;47m▓\033[38;2;55;243;32m▓\033[38;2;79;246;20m▓\033[38;2;70;245;17m▓\033[38;2;129;255;19m▓\033[38;2;76;172;50m▒\033[38;2;0;117;64m░\033[38;2;5;32;18m░\033[38;2;0;0;5m░\033[38;2;40;159;27m▒\033[38;2;75;255;38m▓\033[38;2;154;236;28m▓\033[38;2;15;152;60m▒\033[38;2;0;145;67m▒\033[38;2;2;113;55m░\033[38;2;1;133;57m░\033[38;2;0;100;33m░\033[38;2;0;0;0m░\033[38;2;0;0;9m░\033[38;2;3;6;8m░\033[38;2;33;40;9m░\033[38;2;39;82;21m░\033[38;2;13;162;60m▒\033[38;2;38;194;63m▒\033[38;2;147;244;49m▓\033[38;2;187;255;51m█\033[38;2;113;243;56m▓\033[38;2;71;238;50m▓\033[38;2;36;200;47m▒\033[38;2;19;139;50m▒\033[38;2;2;11;10m░\033[0m'
  '     \033[38;2;0;0;7m░\033[38;2;116;162;19m▒\033[38;2;126;239;44m▓\033[38;2;0;133;60m░\033[38;2;4;153;54m▒\033[38;2;0;160;58m▒\033[38;2;15;192;60m▒\033[38;2;90;205;41m▓\033[38;2;0;126;60m░\033[38;2;2;65;37m░\033[38;2;2;1;6m░\033[38;2;2;0;7m░\033[38;2;2;0;6m░\033[38;2;0;23;11m░\033[38;2;25;104;25m░\033[38;2;90;246;39m▓\033[38;2;19;235;50m▓\033[38;2;25;237;47m▓\033[38;2;26;236;41m▓\033[38;2;99;250;30m▓\033[38;2;77;172;49m▒\033[38;2;0;120;70m░\033[38;2;5;32;18m░\033[38;2;0;0;4m░\033[38;2;36;159;30m▒\033[38;2;74;255;43m▓\033[38;2;156;235;30m▓\033[38;2;15;145;59m▒\033[38;2;0;144;62m▒\033[38;2;3;111;54m░\033[38;2;2;129;58m░\033[38;2;9;110;36m░\033[38;2;59;72;8m░\033[38;2;117;157;21m▒\033[38;2;167;215;25m▓\033[38;2;200;255;30m█\033[38;2;206;255;35m█\033[38;2;175;229;48m▓\033[38;2;94;169;69m▒\033[38;2;95;183;59m▓\033[38;2;36;177;53m▒\033[38;2;0;152;46m▒\033[38;2;0;108;36m░\033[38;2;0;105;49m░\033[38;2;0;138;56m▒\033[38;2;1;9;9m░\033[0m'
  '     \033[38;2;1;2;5m░\033[38;2;14;64;23m░\033[38;2;15;125;50m░\033[38;2;2;130;64m░\033[38;2;2;145;67m▒\033[38;2;0;142;64m▒\033[38;2;15;155;59m▒\033[38;2;93;197;42m▓\033[38;2;0;119;58m░\033[38;2;2;67;37m░\033[38;2;1;18;13m░\033[38;2;1;39;25m░\033[38;2;3;33;22m░\033[38;2;0;2;11m░\033[38;2;44;43;5m░\033[38;2;158;252;22m▓\033[38;2;12;234;52m▓\033[38;2;16;232;50m▓\033[38;2;0;229;54m▒\033[38;2;80;246;37m▓\033[38;2;77;172;47m▒\033[38;2;0;119;71m░\033[38;2;5;32;18m░\033[38;2;0;0;4m░\033[38;2;31;158;32m▒\033[38;2;63;255;46m▓\033[38;2;151;235;26m▓\033[38;2;15;142;56m▒\033[38;2;0;135;60m▒\033[38;2;5;105;52m░\033[38;2;0;115;58m░\033[38;2;55;175;44m▒\033[38;2;217;255;29m█\033[38;2;202;255;44m█\033[38;2;173;235;53m▓\033[38;2;133;206;61m▓\033[38;2;87;180;65m▒\033[38;2;50;160;67m▒\033[38;2;27;147;64m▒\033[38;2;1;121;59m░\033[38;2;0;108;58m░\033[38;2;5;95;54m░\033[38;2;3;97;52m░\033[38;2;2;103;48m░\033[38;2;2;82;36m░\033[38;2;1;8;8m░\033[0m'
  '      \033[38;2;0;0;0m░\033[38;2;0;3;6m░\033[38;2;1;39;18m░\033[38;2;2;76;36m░\033[38;2;0;110;54m░\033[38;2;16;142;61m▒\033[38;2;95;191;43m▓\033[38;2;0;108;55m░\033[38;2;2;48;29m░\033[38;2;1;24;17m░\033[38;2;2;53;32m░\033[38;2;3;46;29m░\033[38;2;0;14;18m░\033[38;2;40;56;7m░\033[38;2;143;243;25m▓\033[38;2;5;208;57m▒\033[38;2;11;227;55m▓\033[38;2;0;228;60m▒\033[38;2;78;244;39m▓\033[38;2;76;172;46m▒\033[38;2;0;120;72m░\033[38;2;4;31;18m░\033[38;2;0;0;4m░\033[38;2;24;156;35m▒\033[38;2;55;255;49m▓\033[38;2;148;234;24m▓\033[38;2;15;141;57m▒\033[38;2;0;115;57m░\033[38;2;3;88;44m░\033[38;2;0;105;51m░\033[38;2;36;165;56m▒\033[38;2;91;178;60m▒\033[38;2;30;142;66m▒\033[38;2;5;126;66m░\033[38;2;0;115;62m░\033[38;2;0;110;61m░\033[38;2;0;106;57m░\033[38;2;0;104;56m░\033[38;2;2;107;55m░\033[38;2;3;105;51m░\033[38;2;1;85;40m░\033[38;2;1;57;26m░\033[38;2;1;16;10m░\033[38;2;0;0;0m░\033[0m'
  '         \033[38;2;3;0;0m░\033[38;2;0;7;5m░\033[38;2;5;45;20m░\033[38;2;20;39;12m░\033[38;2;1;12;10m░\033[38;2;1;1;4m░\033[38;2;1;16;12m░\033[38;2;2;30;20m░\033[38;2;3;53;31m░\033[38;2;0;23;23m░\033[38;2;43;55;7m░\033[38;2;149;244;25m▓\033[38;2;0;175;54m▒\033[38;2;4;187;54m▒\033[38;2;0;204;63m▒\033[38;2;75;240;41m▓\033[38;2;77;175;46m▒\033[38;2;0;118;71m░\033[38;2;4;30;18m░\033[38;2;1;0;2m░\033[38;2;2;130;46m░\033[38;2;27;228;60m▓\033[38;2;145;234;24m▓\033[38;2;15;131;56m▒\033[38;2;0;93;51m░\033[38;2;2;86;45m░\033[38;2;1;83;45m░\033[38;2;0;78;47m░\033[38;2;0;88;55m░\033[38;2;0;100;57m░\033[38;2;1;108;57m░\033[38;2;4;105;56m░\033[38;2;5;97;51m░\033[38;2;3;86;44m░\033[38;2;2;64;32m░\033[38;2;1;48;24m░\033[38;2;1;24;14m░\033[38;2;0;0;0m░\033[0m'
  '                \033[38;2;0;0;0m░\033[38;2;3;11;7m░\033[38;2;0;6;10m░\033[38;2;41;56;8m░\033[38;2;144;234;29m▓\033[38;2;1;158;66m▒\033[38;2;3;163;60m▒\033[38;2;0;161;61m▒\033[38;2;72;211;41m▓\033[38;2;74;159;41m▒\033[38;2;0;88;61m░\033[38;2;4;26;16m░\033[38;2;2;0;2m░\033[38;2;0;130;47m░\033[38;2;10;209;67m▒\033[38;2;138;230;25m▓\033[38;2;15;119;53m░\033[38;2;0;76;50m░\033[38;2;2;91;53m░\033[38;2;2;97;55m░\033[38;2;3;100;54m░\033[38;2;5;97;50m░\033[38;2;3;81;42m░\033[38;2;2;59;31m░\033[38;2;1;42;22m░\033[38;2;1;21;13m░\033[38;2;0;0;2m░\033[0m'
  '                   \033[38;2;11;15;5m░\033[38;2;21;55;16m░\033[38;2;1;74;36m░\033[38;2;5;118;54m░\033[38;2;0;138;68m▒\033[38;2;78;205;45m▓\033[38;2;80;165;42m▒\033[38;2;0;82;62m░\033[38;2;4;24;16m░\033[38;2;2;0;3m░\033[38;2;0;88;41m░\033[38;2;14;184;66m▒\033[38;2;151;245;28m▓\033[38;2;16;137;59m▒\033[38;2;0;102;54m░\033[38;2;2;94;46m░\033[38;2;1;69;34m░\033[38;2;1;36;18m░\033[38;2;1;23;13m░\033[38;2;2;0;0m░\033[0m'
  '                      \033[38;2;3;11;5m░\033[38;2;0;42;24m░\033[38;2;40;83;18m░\033[38;2;39;87;27m░\033[38;2;1;21;21m░\033[38;2;3;7;7m░\033[38;2;3;6;6m░\033[38;2;0;8;10m░\033[38;2;7;42;21m░\033[38;2;79;120;17m▒\033[38;2;9;53;22m░\033[38;2;0;27;15m░\033[38;2;2;0;0m░\033[0m'
)
print_logo() {
  local line color=1
  if [[ -n "${NO_COLOR:-}" || ! -t 1 ]]; then color=0; fi
  printf "\n"
  for line in "${LOGO_LINES[@]}"; do
    if [[ "${color}" == 1 ]]; then
      printf '%b\n' "${line}"
    else
      printf '%b\n' "${line}" | sed $'s/\033\\[[0-9;]*m//g'
    fi
  done
  if [[ "${color}" == 1 ]]; then
    printf "  \033[1;32mHigh Cube\033[0m  \u2014 install\n\n"
  else
    printf "  High Cube  \u2014 install\n\n"
  fi
}

print_logo

USAGE="usage: HC_DEST_USER=<user> ./install.sh <destination-ip> [--role admin|agent|both] [--db true] [--mode install|update]"
DEST_IP=""
COPY_DB=0
# Empty means "ask or auto-detect". --mode forces install or update.
ACTION=""
# Empty means "ask or default to both". --role forces admin, agent, or both.
ROLE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --db)
      shift
      if [[ "${1:-}" != "true" ]]; then
        echo "${USAGE}" >&2
        exit 1
      fi
      COPY_DB=1
      ;;
    --db=true) COPY_DB=1 ;;
    --db=false) COPY_DB=0 ;;
    --mode)
      shift
      ACTION="${1:-}"
      ;;
    --mode=*) ACTION="${1#--mode=}" ;;
    --role)
      shift
      ROLE="${1:-}"
      ;;
    --role=*) ROLE="${1#--role=}" ;;
    -*)
      echo "${USAGE}" >&2
      exit 1
      ;;
    *)
      if [[ -n "${DEST_IP}" ]]; then
        echo "${USAGE}" >&2
        exit 1
      fi
      DEST_IP="$1"
      ;;
  esac
  shift
done
if [[ -z "${DEST_IP}" ]]; then
  echo "${USAGE}" >&2
  exit 1
fi
if [[ -n "${ACTION}" && "${ACTION}" != "install" && "${ACTION}" != "update" ]]; then
  echo "${USAGE}" >&2
  exit 1
fi
if [[ -n "${ROLE}" && "${ROLE}" != "admin" && "${ROLE}" != "agent" && "${ROLE}" != "both" ]]; then
  echo "${USAGE}" >&2
  exit 1
fi
# The destination is interpolated into remote shell commands and the NGINX
# config, so accept only a plain IPv4/IPv6 address or hostname. This rejects
# spaces, quotes, and shell metacharacters that could enable command injection.
if [[ ! "${DEST_IP}" =~ ^[A-Za-z0-9._:-]+$ ]]; then
  echo "error: destination must be an IP address or hostname (got: ${DEST_IP})" >&2
  exit 1
fi

# Destination account and install prefix. High Cube is placed in
# ${INSTALL_DIR}/high-cube. NGINX publishes only ${INSTALL_DIR}/high-cube/public.
# The destination SSH account must be provided via the HC_DEST_USER environment
# variable. There is no default on purpose.
DEST_USER="${HC_DEST_USER:?set HC_DEST_USER to the destination SSH user, e.g. HC_DEST_USER=deploy ./install.sh <ip>}"
# The SSH user is interpolated into remote commands; apply the same guard as the
# destination so a crafted value cannot inject shell metacharacters.
if [[ ! "${DEST_USER}" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "error: HC_DEST_USER must be a plain user name (got: ${DEST_USER})" >&2
  exit 1
fi
INSTALL_DIR="/var/www"

# Agent-only installs point at an existing control panel. These come from the
# environment and may also be prompted for when running on a terminal.
ADMIN_URL="${HC_ADMIN_URL:-}"
AGENT_TOKEN="${HC_AGENT_TOKEN:-}"
NGINX_DEFER="${HC_NGINX_DEFER:-0}"
if [[ "${NGINX_DEFER}" != "0" && "${NGINX_DEFER}" != "1" ]]; then
  echo "error: HC_NGINX_DEFER must be 0 or 1 (got: ${NGINX_DEFER})" >&2
  exit 1
fi

DEST_DIR="${INSTALL_DIR}/high-cube"
PUBLIC_DIR="${DEST_DIR}/public"
# This script lives at the project root.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10 "${DEST_USER}@${DEST_IP}")
RSYNC=(rsync -az -e "ssh -o BatchMode=yes -o ConnectTimeout=10")

# Probe the destination over SSH. Prints "update" when High Cube is already
# installed (binary present or systemd unit registered) and "install" otherwise.
# Prints "unknown" when the host cannot be reached.
detect_action() {
  local probe
  if ! probe="$("${SSH[@]}" "
    if [ -x '${DEST_DIR}/hc-admin' ] || [ -x '${DEST_DIR}/hc-agent' ] \
       || systemctl list-unit-files 2>/dev/null | grep -qE '^hc-(admin|agent)\.service' \
       || systemctl --user list-unit-files 2>/dev/null | grep -qE '^hc-(admin|agent)\.service'; then
      echo installed
    else
      echo fresh
    fi" 2>/dev/null)"; then
    echo "unknown"
    return
  fi
  case "${probe}" in
    installed) echo "update" ;;
    *) echo "install" ;;
  esac
}

# Resolve ACTION: honor --mode, otherwise probe and (when on a terminal) show a
# menu with the detected action preselected.
resolve_action() {
  if [[ -n "${ACTION}" ]]; then
    echo "==> Mode: ${ACTION} (forced by --mode)"
    return
  fi
  echo "==> Probing ${DEST_USER}@${DEST_IP} ..."
  local detected
  detected="$(detect_action)"
  if [[ "${detected}" == "unknown" ]]; then
    echo "    Could not reach the host to detect its state."
    detected="install"
  else
    echo "    Detected state: ${detected}"
  fi
  if [[ ! -t 0 ]]; then
    ACTION="${detected}"
    echo "==> Mode: ${ACTION} (auto, no terminal)"
    return
  fi
  echo "Choose an action for ${DEST_USER}@${DEST_IP}:"
  echo "  1) Install  (fresh setup)"
  echo "  2) Update   (existing installation)"
  echo "  3) Cancel"
  local choice
  read -rp "Selection [default: ${detected}]: " choice
  case "${choice}" in
    1|install|i) ACTION="install" ;;
    2|update|u) ACTION="update" ;;
    3|cancel|c|q) echo "Cancelled."; exit 0 ;;
    "") ACTION="${detected}" ;;
    *) echo "Invalid selection." >&2; exit 1 ;;
  esac
  echo "==> Mode: ${ACTION}"
}

# Resolve ROLE: honor --role, otherwise show a menu (default both) or fall back
# to both when there is no terminal.
resolve_role() {
  if [[ -n "${ROLE}" ]]; then
    echo "==> Role: ${ROLE} (forced by --role)"
    return
  fi
  if [[ ! -t 0 ]]; then
    ROLE="both"
    echo "==> Role: both (default, no terminal)"
    return
  fi
  echo "What to install on ${DEST_USER}@${DEST_IP}:"
  echo "  1) Both   (control panel + worker)   [default]"
  echo "  2) Admin  (control panel only)"
  echo "  3) Agent  (worker only)"
  local choice
  read -rp "Selection [default: both]: " choice
  case "${choice}" in
    1|both|"") ROLE="both" ;;
    2|admin) ROLE="admin" ;;
    3|agent) ROLE="agent" ;;
    *) echo "Invalid selection." >&2; exit 1 ;;
  esac
  echo "==> Role: ${ROLE}"
}

# Agent-only installs need to know which control panel to join and with which
# token. Prompt when on a terminal, otherwise require the environment variables.
ensure_agent_inputs() {
  [[ "${ROLE}" == "agent" ]] || return 0
  if [[ -z "${ADMIN_URL}" && -t 0 ]]; then
    read -rp "Admin WebSocket URL (e.g. ws://ADMIN_IP:3000/api/agents/connect): " ADMIN_URL
  fi
  if [[ -z "${AGENT_TOKEN}" && -t 0 ]]; then
    read -rp "Agent token (created in the admin panel): " AGENT_TOKEN
  fi
  if [[ -z "${ADMIN_URL}" || -z "${AGENT_TOKEN}" ]]; then
    echo "error: an agent-only install needs HC_ADMIN_URL and HC_AGENT_TOKEN" >&2
    exit 1
  fi
}

# Run a preflight on the destination to confirm it meets the requirements for
# the chosen role before building and copying anything.
preflight() {
  echo "==> Preflight: checking ${DEST_USER}@${DEST_IP} for role '${ROLE}'"
  local rc=0
  set +e
  "${SSH[@]}" "ROLE='${ROLE}' INSTALL_DIR='${INSTALL_DIR}' bash -s" <<'PRE'
set -u
ROLE="${ROLE:-both}"
INSTALL_DIR="${INSTALL_DIR:-/var/www}"
fail=0
ok()   { printf '    [ OK ] %s\n' "$1"; }
warn() { printf '    [WARN] %s\n' "$1"; }
bad()  { printf '    [FAIL] %s\n' "$1"; fail=1; }

want_admin=0; want_agent=0
case "${ROLE}" in
  admin) want_admin=1 ;;
  agent) want_agent=1 ;;
  *) want_admin=1; want_agent=1 ;;
esac

# Common requirements.
[ "$(uname -s)" = "Linux" ] && ok "Linux kernel" || bad "operating system is not Linux"
[ -d /run/systemd/system ] && ok "systemd is running" || bad "systemd is required"
arch="$(uname -m)"
[ "${arch}" = "x86_64" ] && ok "architecture x86_64" \
  || bad "architecture ${arch} unsupported (binaries are built for x86_64)"
command -v openssl >/dev/null 2>&1 && ok "openssl present" || bad "openssl is missing"
if sudo -n true 2>/dev/null; then
  ok "passwordless sudo"
elif [ -t 0 ]; then
  warn "sudo will prompt for a password (run from a terminal)"
else
  warn "sudo is not passwordless; some steps may fall back to user services"
fi
avail_kb="$(df -Pk "${INSTALL_DIR%/*}" 2>/dev/null | awk 'NR==2{print $4}')"
if [ -n "${avail_kb}" ] && [ "${avail_kb}" -ge 1048576 ]; then
  ok "at least 1 GB free on ${INSTALL_DIR%/*}"
else
  warn "less than 1 GB free on ${INSTALL_DIR%/*}"
fi

if [ "${want_admin}" = 1 ]; then
  command -v python3 >/dev/null 2>&1 && ok "python3 present" || bad "python3 is required for the admin role"
  command -v apt-get >/dev/null 2>&1 && ok "apt-get present" \
    || warn "apt-get not found; install nginx and certbot manually"
  command -v curl >/dev/null 2>&1 && ok "curl present" || warn "curl missing (post-install checks)"
  if command -v ss >/dev/null 2>&1; then
    ports="$(ss -ltn 2>/dev/null | awk '{print $4}')"
    printf '%s\n' "${ports}" | grep -qE '[:.]3000$' && warn "port 3000 already in use" || ok "port 3000 is free"
    printf '%s\n' "${ports}" | grep -qE '[:.]80$'   && warn "port 80 already in use (will be reconfigured)" || ok "port 80 is free"
  fi
fi

if [ "${want_agent}" = 1 ]; then
  if command -v docker >/dev/null 2>&1; then
    ok "docker CLI present"
    if docker info >/dev/null 2>&1; then
      ok "docker daemon reachable"
    elif sudo -n docker info >/dev/null 2>&1; then
      warn "docker needs sudo for this user; it will run as a system service"
    else
      bad "docker daemon is not reachable"
    fi
    getent group docker >/dev/null 2>&1 && ok "docker group exists" \
      || warn "docker group missing; the agent service may fail to access docker"
  else
    bad "docker is not installed (required for the agent role)"
  fi
fi

if [ "${fail}" -ne 0 ]; then
  echo "    preflight: the host is missing one or more requirements"
  exit 1
fi
exit 0
PRE
  rc=$?
  set -e
  if [[ "${rc}" -ne 0 ]]; then
    echo "FAIL preflight: ${DEST_USER}@${DEST_IP} is not ready for role '${ROLE}'." >&2
    echo "     Fix the [FAIL] items above and run install again." >&2
    exit 1
  fi
  echo "OK  Preflight passed"
}

resolve_action
resolve_role
ensure_agent_inputs

# --db only makes sense when the admin database lives on this host.
if [[ "${ROLE}" == "agent" && "${COPY_DB}" -eq 1 ]]; then
  echo "==> Ignoring --db: the agent role has no database to copy."
  COPY_DB=0
fi

if [[ "${ACTION}" == "install" ]]; then
  preflight
fi

stage() {
  local name="$1"
  shift
  echo "==> ${name}"
  if "$@"; then
    echo "OK  ${name}"
  else
    echo "FAIL ${name}" >&2
    exit 1
  fi
}

require_file() {
  local path="$1"
  [[ -f "${path}" ]]
}

echo "==> 1. Build binaries"
build_targets=()
[[ "${ROLE}" != "agent" ]] && build_targets+=(hc-admin)
[[ "${ROLE}" != "admin" ]] && build_targets+=(hc-agent)
cmake --build "${ROOT}/build" --target "${build_targets[@]}" -j"$(nproc)"
if [[ "${ROLE}" != "agent" ]]; then
  stage "1. hc-admin exists" require_file "${ROOT}/build/hc-admin"
fi
if [[ "${ROLE}" != "admin" ]]; then
  stage "1. hc-agent exists" require_file "${ROOT}/build/hc-agent"
fi

echo "==> 2. Build admin frontend"
if [[ "${ROLE}" != "agent" ]]; then
  if [[ -f "${ROOT}/src/admin/frontend/package.json" ]] && command -v npm >/dev/null 2>&1; then
    (cd "${ROOT}/src/admin/frontend" && npm run build)
  fi
  stage "2. Frontend dist" require_file "${ROOT}/src/admin/frontend/dist/index.html"
  grep -q "High Cube" "${ROOT}/src/admin/frontend/dist/index.html"
else
  echo "OK  2. Frontend not needed for the agent role"
fi

LIB_STAGE="$(mktemp -d)"
REMOTE_SCRIPT="$(mktemp)"
DB_STAGE=""
cleanup() { rm -rf "${LIB_STAGE}" "${REMOTE_SCRIPT}" ${DB_STAGE:+"${DB_STAGE}"}; }
trap cleanup EXIT
for lib in libboost_filesystem.so.1.83.0 libspdlog.so.1.12 libfmt.so.9 libprotobuf.so.32; do
  if [[ ! -e "/lib/x86_64-linux-gnu/${lib}" ]]; then
    echo "FAIL 3. Local runtime library ${lib} is missing" >&2
    exit 1
  fi
  # Copy the real file under the SONAME. These libraries are often symlinks.
  cp -L "/lib/x86_64-linux-gnu/${lib}" "${LIB_STAGE}/${lib}"
done
echo "OK  3. Runtime libraries staged"

echo "==> 4. SSH to ${DEST_USER}@${DEST_IP} and prepare ${DEST_DIR}"
"${SSH[@]}" "echo connected"
install_dirs="'${DEST_DIR}/lib' '${DEST_DIR}/data' '${DEST_DIR}/agent-data' '${DEST_DIR}/nginx-sites' '${PUBLIC_DIR}' '${DEST_DIR}/migrations' '${DEST_DIR}/src/admin/frontend/dist'"
if ! "${SSH[@]}" "mkdir -p ${install_dirs}"; then
  create_cmd="mkdir -p ${install_dirs} && chown -R '${DEST_USER}:${DEST_USER}' '${DEST_DIR}' && chmod 755 '${DEST_DIR}'"
  if "${SSH[@]}" "sudo -n bash -c $(printf '%q' "${create_cmd}")"; then
    :
  elif [[ -t 0 ]]; then
    echo "==> sudo password required to create ${DEST_DIR}"
    ssh -tt -o ConnectTimeout=10 "${DEST_USER}@${DEST_IP}" "sudo bash -c $(printf '%q' "${create_cmd}")"
  else
    echo "FAIL 4. Cannot create ${DEST_DIR}. Run ./install.sh ${DEST_IP} from a terminal and enter the sudo password." >&2
    exit 1
  fi
fi
echo "OK  4. SSH and directories"

echo "==> 5. Copy binaries, frontend, migrations, and libraries"
if [[ "${ROLE}" == "admin" ]]; then
  "${RSYNC[@]}" "${ROOT}/build/hc-admin" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/"
elif [[ "${ROLE}" == "agent" ]]; then
  "${RSYNC[@]}" "${ROOT}/build/hc-agent" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/"
else
  "${RSYNC[@]}" "${ROOT}/build/hc-admin" "${ROOT}/build/hc-agent" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/"
fi
"${RSYNC[@]}" "${LIB_STAGE}/" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/lib/"
if [[ "${ROLE}" != "agent" ]]; then
  "${RSYNC[@]}" --delete "${ROOT}/migrations/" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/migrations/"
  "${RSYNC[@]}" --delete "${ROOT}/src/admin/frontend/dist/" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/src/admin/frontend/dist/"
fi
echo "OK  5. Files copied"

LOCAL_AGENT_TOKEN=""
if [[ "${COPY_DB}" -eq 1 ]]; then
  echo "==> 5b. Snapshot local SQLite database"
  LOCAL_DB="${ROOT}/data/highcube.db"
  if [[ ! -r "${LOCAL_DB}" ]]; then
    echo "FAIL 5b. Cannot read ${LOCAL_DB}" >&2
    exit 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "FAIL 5b. python3 is required to copy a consistent database" >&2
    exit 1
  fi
  DB_STAGE="$(mktemp -d)"
  # Fold the WAL into one file so the copy does not depend on -wal/-shm.
  python3 - "${LOCAL_DB}" "${DB_STAGE}/highcube.db" <<'PY'
import sqlite3, sys
source = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
target = sqlite3.connect(sys.argv[2])
source.backup(target)
target.close()
source.close()
PY
  if [[ ! -s "${DB_STAGE}/highcube.db" ]]; then
    echo "FAIL 5b. Database backup is empty" >&2
    exit 1
  fi
  if [[ -f "${ROOT}/hc-agent.conf" ]]; then
    LOCAL_AGENT_TOKEN="$(sed -n 's/^agent_token=//p' "${ROOT}/hc-agent.conf" | head -1)"
  fi
  "${SSH[@]}" "systemctl --user stop hc-admin.service hc-agent.service 2>/dev/null || true; sudo -n systemctl stop hc-admin.service hc-agent.service 2>/dev/null || true"
  "${RSYNC[@]}" "${DB_STAGE}/highcube.db" "${DEST_USER}@${DEST_IP}:${DEST_DIR}/data/highcube.db"
  "${SSH[@]}" "rm -f '${DEST_DIR}/data/highcube.db-wal' '${DEST_DIR}/data/highcube.db-shm'"
  echo "OK  5b. Local database copied"
fi

cat > "${REMOTE_SCRIPT}" <<'REMOTE'
set -euo pipefail
DEST_IP="$1"
DEST_DIR="$2"
PUBLIC_DIR="${DEST_DIR}/public"
MODE="${3:-nopw}"
COPY_DB="${4:-0}"
PRESET_TOKEN="${5:-}"
ROLE="${6:-both}"
ADMIN_URL="${7:-}"
AGENT_TOKEN="${8:-}"
NGINX_DEFER="${9:-0}"
cd "${DEST_DIR}"

want_admin=0; want_agent=0
case "${ROLE}" in
  admin) want_admin=1 ;;
  agent) want_agent=1 ;;
  *) want_admin=1; want_agent=1 ;;
esac

stage() {
  local name="$1"
  shift
  echo "==> ${name}"
  if "$@"; then
    echo "OK  ${name}"
  else
    echo "FAIL ${name}" >&2
    exit 1
  fi
}

as_root() {
  if [[ "${MODE}" == interactive ]]; then
    sudo "$@"
  else
    sudo -n "$@"
  fi
}

echo "==> 6. Binaries load on the destination"
if [[ "${want_admin}" == 1 ]]; then
  chmod 755 hc-admin
  if LD_LIBRARY_PATH="${DEST_DIR}/lib" ldd ./hc-admin | grep -q "not found"; then
    echo "FAIL 6. hc-admin is missing shared libraries" >&2
    LD_LIBRARY_PATH="${DEST_DIR}/lib" ldd ./hc-admin >&2 || true
    exit 1
  fi
fi
if [[ "${want_agent}" == 1 ]]; then
  chmod 755 hc-agent
  if LD_LIBRARY_PATH="${DEST_DIR}/lib" ldd ./hc-agent | grep -q "not found"; then
    echo "FAIL 6. hc-agent is missing shared libraries" >&2
    LD_LIBRARY_PATH="${DEST_DIR}/lib" ldd ./hc-agent >&2 || true
    exit 1
  fi
fi
echo "OK  6. Shared libraries resolve"

echo "==> 7. Configuration files"
if [[ "${want_admin}" == 1 ]]; then
  SECRET=""
  CERTBOT_EMAIL=""
  if [[ -f hc-admin.conf ]]; then
    SECRET="$(sed -n 's/^session_secret=//p' hc-admin.conf | head -1)"
    CERTBOT_EMAIL="$(sed -n 's/^certbot_email=//p' hc-admin.conf | head -1)"
  fi
  if [[ -z "${SECRET}" || "${SECRET}" == change-me* ]]; then
    SECRET="$(openssl rand -hex 32)"
  fi
  if [[ -z "${CERTBOT_EMAIL}" && -f data/highcube.db ]]; then
    CERTBOT_EMAIL="$(python3 -c "import sqlite3; row=sqlite3.connect('data/highcube.db').execute(\"select u.email from users u join roles r on r.id=u.role_id where r.code='admin' and u.email not like '%@highcube.local' order by u.id limit 1\").fetchone(); print(row[0] if row else '')" 2>/dev/null || true)"
  fi
  cat > hc-admin.conf <<EOF
http_port=3000
sqlite_path=data/highcube.db
public_url=http://${DEST_IP}
frontend_dist=src/admin/frontend/dist
migrations_dir=migrations
nginx_sites_dir=${DEST_DIR}/nginx-sites
certbot_email=${CERTBOT_EMAIL}
session_secret=${SECRET}
log_file=logs/log.txt
log_retention_days=7
log_level=info
EOF
  chmod 600 hc-admin.conf
  grep -q "^session_secret=${SECRET}$" hc-admin.conf
fi

TOKEN=""
HOSTNAME="$(hostname -s)"
if [[ "${want_agent}" == 1 ]]; then
  if [[ "${want_admin}" == 1 ]]; then
    # Co-located worker: talk to the local admin and use a local token.
    AGENT_ADMIN_URL="ws://127.0.0.1:3000/api/agents/connect"
    if [[ -n "${PRESET_TOKEN}" ]]; then
      TOKEN="${PRESET_TOKEN}"
    elif [[ -f hc-agent.conf ]]; then
      TOKEN="$(sed -n 's/^agent_token=//p' hc-agent.conf | head -1)"
    fi
    if [[ -z "${TOKEN}" ]]; then
      TOKEN="$(openssl rand -hex 32)"
    fi
  else
    # Standalone worker: talk to a remote admin with the token from its panel.
    AGENT_ADMIN_URL="${ADMIN_URL}"
    TOKEN="${AGENT_TOKEN}"
  fi
  cat > hc-agent.conf <<EOF
admin_url=${AGENT_ADMIN_URL}
agent_token=${TOKEN}
server_name=${HOSTNAME}
server_ip=${DEST_IP}
data_dir=${DEST_DIR}/agent-data
EOF
  chmod 600 hc-agent.conf
  grep -q "^agent_token=${TOKEN}$" hc-agent.conf
fi
echo "OK  7. Configuration files"

if [[ "${want_admin}" == 1 ]]; then
  echo "==> 8. Database migrations"
  export LD_LIBRARY_PATH="${DEST_DIR}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
  applied=0
  if [[ -f data/highcube.db ]]; then
    applied="$(python3 -c "import sqlite3; print(sqlite3.connect('data/highcube.db').execute('select count(*) from schema_migrations').fetchone()[0])" 2>/dev/null || echo 0)"
  fi
  if [[ "${applied}" -lt 5 ]]; then
    set +e
    timeout 20 ./hc-admin --service --config "${DEST_DIR}/hc-admin.conf" >/tmp/hc-admin-migrate.log 2>&1
    set -e
    if [[ -f /tmp/hc-admin-migrate.log ]] && grep -q "error while loading shared libraries" /tmp/hc-admin-migrate.log; then
      echo "FAIL 8. hc-admin failed to start" >&2
      cat /tmp/hc-admin-migrate.log >&2
      exit 1
    fi
  fi
  python3 - "${DEST_DIR}" <<'PY'
import pathlib, sqlite3, sys
db = pathlib.Path(sys.argv[1]) / "data" / "highcube.db"
conn = sqlite3.connect(db)
n = conn.execute("SELECT COUNT(*) FROM schema_migrations").fetchone()[0]
if n < 5:
    raise SystemExit(f"expected at least 5 migrations, found {n}")
print(f"migrations={n}")
PY
  echo "OK  8. Database migrations"
fi

if [[ "${want_admin}" == 1 ]]; then
  echo "==> 9. Administrator and server records"
  # Create the first administrator when the database has no users. Works for a
  # fresh install and for a copied database (which already has users).
  python3 - "${DEST_DIR}" <<'PY'
import pathlib, sqlite3, sys
dest = sys.argv[1]
db_path = pathlib.Path(dest) / "data" / "highcube.db"
conn = sqlite3.connect(db_path)
conn.row_factory = sqlite3.Row
role = conn.execute("SELECT id FROM roles WHERE code='admin'").fetchone()
if role is None:
    raise SystemExit("admin role missing after migrations")
users = conn.execute("SELECT COUNT(*) AS n FROM users").fetchone()["n"]
password_file = pathlib.Path(dest) / ".first-admin.txt"
if users == 0:
    import crypt
    password = pathlib.Path("/proc/sys/kernel/random/uuid").read_text().strip()
    hashed = crypt.crypt(password, crypt.mksalt(crypt.METHOD_BLOWFISH))
    conn.execute(
        "INSERT INTO users (email, password_hash, name, locale, role_id) VALUES (?,?,?,?,?)",
        ("admin@highcube.local", hashed, "Administrator", "es", role["id"]),
    )
    password_file.write_text("email=admin@highcube.local\npassword=%s\n" % password)
    password_file.chmod(0o600)
    print("FIRST_ADMIN_CREATED")
conn.commit()
users = conn.execute("SELECT COUNT(*) AS n FROM users").fetchone()["n"]
if users < 1:
    raise SystemExit("no administrator user")
print("users=%s" % users)
PY
  # Register the co-located worker's server record on a fresh database.
  if [[ "${want_agent}" == 1 && "${COPY_DB}" != "1" ]]; then
    python3 - "${DEST_DIR}" "${TOKEN}" "${HOSTNAME}" "${DEST_IP}" <<'PY'
import hashlib, pathlib, sqlite3, sys
dest, token, name, ip = sys.argv[1:]
db_path = pathlib.Path(dest) / "data" / "highcube.db"
conn = sqlite3.connect(db_path)
conn.row_factory = sqlite3.Row
token_hash = hashlib.sha256(token.encode()).hexdigest()
# The admin identifies agents by token, and the name may have been changed in the UI.
row = conn.execute("SELECT id FROM servers WHERE agent_token_hash=?", (token_hash,)).fetchone()
if row is None:
    row = conn.execute("SELECT id FROM servers WHERE name=?", (name,)).fetchone()
if row is None:
    conn.execute(
        "INSERT INTO servers (name, ip, agent_token_hash, status) VALUES (?,?,?,?)",
        (name, ip, token_hash, "pending"),
    )
else:
    conn.execute(
        "UPDATE servers SET ip=?, agent_token_hash=? WHERE id=?",
        (ip, token_hash, row["id"]),
    )
conn.commit()
print("server=%s" % name)
PY
  fi
  echo "OK  9. Administrator and server records"
fi

if [[ "${want_admin}" == 1 ]]; then
  echo "==> 10. NGINX"
  if ! sudo -n true 2>/dev/null && [[ "${MODE}" != interactive ]]; then
    echo "NEED_SUDO"
    exit 42
  fi
  if ! command -v nginx >/dev/null 2>&1 || ! command -v certbot >/dev/null 2>&1; then
    as_root apt-get update -y
    if [[ "${NGINX_DEFER}" == 1 ]]; then
      # The package would start NGINX right away and fail on the busy ports.
      as_root sh -c 'printf "#!/bin/sh\nexit 101\n" > /usr/sbin/policy-rc.d && chmod 755 /usr/sbin/policy-rc.d'
      set +e
      as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends nginx certbot
      apt_rc=$?
      set -e
      as_root rm -f /usr/sbin/policy-rc.d
      [[ "${apt_rc}" -eq 0 ]]
    else
      as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends nginx certbot
    fi
  fi
  command -v nginx >/dev/null 2>&1
  command -v certbot >/dev/null 2>&1
  if [[ "${NGINX_DEFER}" == 1 ]]; then
    # Keep it off at boot too, so it never grabs 80/443 before the other proxy.
    as_root systemctl disable nginx
    as_root systemctl stop nginx 2>/dev/null || true
  else
    as_root systemctl enable nginx
    as_root systemctl start nginx
    as_root systemctl is-active nginx
  fi

  # Publish only the frontend. The rest of the install dir holds secrets and is not the web root.
  as_root mkdir -p "${PUBLIC_DIR}"
  as_root rsync -a --delete "${DEST_DIR}/src/admin/frontend/dist/" "${PUBLIC_DIR}/"
  as_root chown -R www-data:www-data "${PUBLIC_DIR}"
  as_root chmod 755 "${DEST_DIR}"
  as_root tee /etc/nginx/sites-available/highcube-panel.conf >/dev/null <<EOF
# Managed by high-cube/install.sh. Serves the control panel on this host IP.
server {
  listen 80;
  listen [::]:80;
  server_name ${DEST_IP};
  root ${PUBLIC_DIR};
  index index.html;

  location /api/ {
    proxy_pass http://127.0.0.1:3000;
    proxy_http_version 1.1;
    proxy_set_header Upgrade \$http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header Host \$host;
    proxy_set_header X-Real-IP \$remote_addr;
    proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto \$scheme;
    proxy_read_timeout 3600s;
  }

  location / {
    try_files \$uri /index.html;
  }
}
EOF
  as_root ln -sfn /etc/nginx/sites-available/highcube-panel.conf /etc/nginx/sites-enabled/highcube-panel.conf
  # hc-admin writes vhosts here as the service user. Nginx only reads sites-enabled,
  # so the subdomain server_name never reached certbot until this include existed.
  mkdir -p "${DEST_DIR}/nginx-sites" "${DEST_DIR}/acme"
  as_root tee /etc/nginx/sites-enabled/highcube-apps.conf >/dev/null <<EOF
# Application vhosts written by hc-admin. Do not include highcube-admin.conf:
# the panel already owns the default server.
include ${DEST_DIR}/nginx-sites/highcube-app-*.conf;
include ${DEST_DIR}/nginx-sites/highcube-challenge-*.conf;
EOF
  NGINX_BIN="$(command -v nginx)"
  CERTBOT_BIN="$(command -v certbot)"
  as_root tee /etc/sudoers.d/highcube >/dev/null <<EOF
${USER} ALL=(root) NOPASSWD: ${NGINX_BIN} -s reload
${USER} ALL=(root) NOPASSWD: ${CERTBOT_BIN}
EOF
  as_root chmod 440 /etc/sudoers.d/highcube
  as_root visudo -cf /etc/sudoers.d/highcube
  if ! as_root nginx -t; then
    as_root rm -f /etc/nginx/sites-enabled/highcube-apps.conf
    echo "FAIL 10. nginx -t rejected the configuration; leaving the previous config loaded" >&2
    exit 1
  fi
  if [[ "${NGINX_DEFER}" == 1 ]]; then
    echo "OK  10. NGINX installed and tested; left stopped and disabled (HC_NGINX_DEFER=1)"
  else
    as_root systemctl reload nginx
    echo "OK  10. NGINX installed, tested, and reloaded"
  fi
fi

echo "==> 11. Services"
# The admin service only needs the docker group when a worker shares this host.
if [[ "${want_agent}" == 1 ]]; then
  ADMIN_SUP_LINE="SupplementaryGroups=docker"
else
  ADMIN_SUP_LINE=""
fi
UNIT_ADMIN="[Unit]
Description=High Cube Admin
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${USER}
Group=${USER}
${ADMIN_SUP_LINE}
WorkingDirectory=${DEST_DIR}
Environment=LD_LIBRARY_PATH=${DEST_DIR}/lib
ExecStart=${DEST_DIR}/hc-admin --service --config ${DEST_DIR}/hc-admin.conf
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"
UNIT_AGENT="[Unit]
Description=High Cube Agent
After=network-online.target docker.service
Wants=network-online.target
Requires=docker.service

[Service]
Type=simple
User=${USER}
Group=${USER}
SupplementaryGroups=docker
WorkingDirectory=${DEST_DIR}
Environment=LD_LIBRARY_PATH=${DEST_DIR}/lib
ExecStart=${DEST_DIR}/hc-agent --service --config ${DEST_DIR}/hc-agent.conf
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
"

if as_root true 2>/dev/null; then
  units=()
  if [[ "${want_admin}" == 1 ]]; then
    printf '%s\n' "${UNIT_ADMIN}" | as_root tee /etc/systemd/system/hc-admin.service >/dev/null
    units+=(hc-admin.service)
  fi
  if [[ "${want_agent}" == 1 ]]; then
    printf '%s\n' "${UNIT_AGENT}" | as_root tee /etc/systemd/system/hc-agent.service >/dev/null
    units+=(hc-agent.service)
  fi
  as_root systemctl daemon-reload
  as_root systemctl enable "${units[@]}"
  as_root systemctl restart "${units[@]}"
  sleep 2
  for u in "${units[@]}"; do as_root systemctl is-active "${u}"; done
else
  mkdir -p "${HOME}/.config/systemd/user"
  units=()
  if [[ "${want_admin}" == 1 ]]; then
    printf '%s\n' "${UNIT_ADMIN}" | sed '/^User=/d;/^Group=/d;/^SupplementaryGroups=/d;s/WantedBy=multi-user.target/WantedBy=default.target/' > "${HOME}/.config/systemd/user/hc-admin.service"
    units+=(hc-admin.service)
  fi
  if [[ "${want_agent}" == 1 ]]; then
    printf '%s\n' "${UNIT_AGENT}" | sed '/^User=/d;/^Group=/d;/^SupplementaryGroups=/d;/^Requires=docker.service$/d;s/ docker.service//;s/WantedBy=multi-user.target/WantedBy=default.target/' > "${HOME}/.config/systemd/user/hc-agent.service"
    units+=(hc-agent.service)
  fi
  systemctl --user daemon-reload
  loginctl enable-linger "${USER}" 2>/dev/null || true
  systemctl --user enable "${units[@]}"
  systemctl --user restart "${units[@]}"
  sleep 2
  for u in "${units[@]}"; do systemctl --user is-active "${u}"; done
fi
echo "OK  11. Services are active"

if [[ "${want_admin}" == 1 ]]; then
  echo "==> 12. Panel, API, and agent"
  api_code="$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:3000/api/settings/base-domains || true)"
  if [[ "${api_code}" != "401" ]]; then
    echo "FAIL 12. API on :3000 returned ${api_code}, expected 401" >&2
    exit 1
  fi
  if [[ "${NGINX_DEFER}" != 1 ]]; then
    panel="$(curl -fsS -H "Host: ${DEST_IP}" http://127.0.0.1/)"
    printf '%s' "${panel}" | grep -q "High Cube"
    asset="$(printf '%s' "${panel}" | sed -n 's/.*src="\(\/assets\/[^"]*\)".*/\1/p' | head -1)"
    if [[ -z "${asset}" ]]; then
      echo "FAIL 12. Panel HTML has no frontend asset" >&2
      exit 1
    fi
    curl -fsS -o /dev/null -H "Host: ${DEST_IP}" "http://127.0.0.1${asset}"
  fi
  if [[ "${want_agent}" == 1 ]]; then
    ready=0
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      if journalctl --user -u hc-admin --since "2 min ago" --no-pager 2>/dev/null | grep -q "agent registered"; then
        ready=1
        break
      fi
      if journalctl -u hc-admin --since "2 min ago" --no-pager 2>/dev/null | grep -q "agent registered"; then
        ready=1
        break
      fi
      sleep 1
    done
    if [[ "${ready}" != 1 ]]; then
      echo "FAIL 12. Agent did not register with hc-admin" >&2
      journalctl --user -u hc-admin -u hc-agent --since "2 min ago" --no-pager >&2 || true
      exit 1
    fi
  fi
  echo "OK  12. Panel, API, and agent"
elif [[ "${want_agent}" == 1 ]]; then
  echo "==> 12. Agent health"
  # Agent-only: the service is already confirmed active above. Surface whether
  # it has connected to the remote admin, but do not fail if it is still trying.
  if journalctl --user -u hc-agent --since "2 min ago" --no-pager 2>/dev/null | grep -qiE "connected|registered"; then
    echo "OK  12. Agent connected to ${ADMIN_URL}"
  elif journalctl -u hc-agent --since "2 min ago" --no-pager 2>/dev/null | grep -qiE "connected|registered"; then
    echo "OK  12. Agent connected to ${ADMIN_URL}"
  else
    echo "OK  12. Agent service running; verify it appears in the admin panel"
  fi
fi
REMOTE

run_remote() {
  local mode="$1"
  local token_q role_q admin_url_q agent_token_q
  token_q="$(printf '%q' "${LOCAL_AGENT_TOKEN}")"
  role_q="$(printf '%q' "${ROLE}")"
  admin_url_q="$(printf '%q' "${ADMIN_URL}")"
  agent_token_q="$(printf '%q' "${AGENT_TOKEN}")"
  if [[ "${mode}" == interactive ]]; then
    # The script must not occupy stdin, because sudo reads the password from it.
    "${RSYNC[@]}" "${REMOTE_SCRIPT}" "${DEST_USER}@${DEST_IP}:/tmp/hc-update-remote.sh"
    ssh -tt -o ConnectTimeout=10 "${DEST_USER}@${DEST_IP}" \
      "bash /tmp/hc-update-remote.sh '${DEST_IP}' '${DEST_DIR}' interactive '${COPY_DB}' ${token_q} ${role_q} ${admin_url_q} ${agent_token_q} '${NGINX_DEFER}'"
  else
    "${SSH[@]}" "bash -s -- '${DEST_IP}' '${DEST_DIR}' nopw '${COPY_DB}' ${token_q} ${role_q} ${admin_url_q} ${agent_token_q} '${NGINX_DEFER}'" < "${REMOTE_SCRIPT}"
  fi
}

set +e
run_remote nopw
remote_rc=$?
set -e
if [[ "${remote_rc}" -eq 42 ]]; then
  if [[ ! -t 0 ]]; then
    echo "FAIL 10. NGINX needs sudo, and this shell cannot prompt for the password." >&2
    echo "     Run ./install.sh ${DEST_IP} from a terminal and enter the sudo password." >&2
    exit 1
  fi
  echo "==> sudo password required on ${DEST_IP}"
  run_remote interactive
  remote_rc=$?
fi
if [[ "${remote_rc}" -ne 0 ]]; then
  echo "FAIL deploy (remote exit ${remote_rc})" >&2
  exit "${remote_rc}"
fi

if [[ "${ROLE}" != "agent" && "${NGINX_DEFER}" == 1 ]]; then
  echo "OK  13. Skipped: NGINX is deferred (HC_NGINX_DEFER=1)"
  echo "Deploy succeeded (role: ${ROLE}). API on http://${DEST_IP}:3000/; start NGINX to serve the panel."
elif [[ "${ROLE}" != "agent" ]]; then
  echo "==> 13. Check the panel from this machine"
  panel_file="$(mktemp)"
  panel_code="$(curl -s -o "${panel_file}" -w '%{http_code}' "http://${DEST_IP}/" || true)"
  if [[ "${panel_code}" != "200" ]] || ! grep -q "High Cube" "${panel_file}"; then
    echo "FAIL 13. http://${DEST_IP}/ returned ${panel_code}" >&2
    rm -f "${panel_file}"
    exit 1
  fi
  rm -f "${panel_file}"
  api_code="$(curl -s -o /dev/null -w '%{http_code}' "http://${DEST_IP}/api/settings/base-domains" || true)"
  if [[ "${api_code}" != "401" ]]; then
    echo "FAIL 13. http://${DEST_IP}/api returned ${api_code}, expected 401" >&2
    exit 1
  fi
  echo "OK  13. Panel and API reachable at http://${DEST_IP}/"
  echo "Deploy succeeded (role: ${ROLE}): http://${DEST_IP}/"
  echo "First-admin credentials, when created, are in ${DEST_DIR}/.first-admin.txt on the server."
else
  echo "Agent installed on ${DEST_IP}. It connects to ${ADMIN_URL}."
  echo "Confirm the server turns healthy in the admin panel."
fi
