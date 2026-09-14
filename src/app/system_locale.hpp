#pragma once

#include <SDL3/SDL.h>

#include <string_view>

namespace pulseforge::detail {

enum class SystemUiLanguage {
    english,
    portuguese,
    spanish,
    french,
    german,
    italian,
    dutch,
    polish,
    russian,
    japanese,
    korean,
    chinese,
};

[[nodiscard]] inline bool locale_language_is(
    const char* value,
    const std::string_view expected
) noexcept {
    if (value == nullptr) return false;
    const std::string_view language{value};
    if (language.size() < expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        char actual = language[index];
        if (actual >= 'A' && actual <= 'Z') {
            actual = static_cast<char>(actual - 'A' + 'a');
        }
        if (actual != expected[index]) return false;
    }
    return language.size() == expected.size()
        || language[expected.size()] == '-'
        || language[expected.size()] == '_';
}

// SDL forwards the operating system's preferred locale list on Windows,
// macOS, Linux, Android and iOS. We pick the first language for which the
// engine has strings and fall back to English when the OS locale is not yet
// translated. No location/IP data is used.
[[nodiscard]] inline SystemUiLanguage system_ui_language() noexcept {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (locales == nullptr) return SystemUiLanguage::english;

    SystemUiLanguage selected = SystemUiLanguage::english;
    bool found = false;
    for (int index = 0; index < count && !found; ++index) {
        const char* const language = locales[index] == nullptr
            ? nullptr
            : locales[index]->language;
        if (locale_language_is(language, "pt")) {
            selected = SystemUiLanguage::portuguese;
            found = true;
        } else if (locale_language_is(language, "es")) {
            selected = SystemUiLanguage::spanish;
            found = true;
        } else if (locale_language_is(language, "fr")) {
            selected = SystemUiLanguage::french;
            found = true;
        } else if (locale_language_is(language, "de")) {
            selected = SystemUiLanguage::german;
            found = true;
        } else if (locale_language_is(language, "it")) {
            selected = SystemUiLanguage::italian;
            found = true;
        } else if (locale_language_is(language, "nl")) {
            selected = SystemUiLanguage::dutch;
            found = true;
        } else if (locale_language_is(language, "pl")) {
            selected = SystemUiLanguage::polish;
            found = true;
        } else if (locale_language_is(language, "ru")) {
            selected = SystemUiLanguage::russian;
            found = true;
        } else if (locale_language_is(language, "ja")) {
            selected = SystemUiLanguage::japanese;
            found = true;
        } else if (locale_language_is(language, "ko")) {
            selected = SystemUiLanguage::korean;
            found = true;
        } else if (locale_language_is(language, "zh")) {
            selected = SystemUiLanguage::chinese;
            found = true;
        } else if (locale_language_is(language, "en")) {
            selected = SystemUiLanguage::english;
            found = true;
        }
    }
    SDL_free(locales);
    return selected;
}

struct CompleteUiStrings {
    std::string_view yes;
    std::string_view no;
    std::string_view has_one_mod;
    std::string_view has_many_mods;
    std::string_view files_suffix;
    std::string_view install_now;
    std::string_view transfer_hint;
    std::string_view install_failed;
    std::string_view preserved_hint;
    std::string_view preparing;
    std::string_view cancelling;
    std::string_view mods_label;
    std::string_view files_label;
};

[[nodiscard]] inline CompleteUiStrings complete_ui_strings(
    const SystemUiLanguage language = system_ui_language()
) noexcept {
    switch (language) {
    case SystemUiLanguage::portuguese:
        return {
            "Sim", "Não",
            "O PulseForge Complete tem 1 mod por instalar/atualizar (",
            "O PulseForge Complete tem {count} mods por instalar/atualizar (",
            " ficheiros).",
            "Pretende instalar os mods agora?",
            "Os ficheiros são transferidos diretamente do Google Drive para a pasta de mods. Se escolher Não, o PulseForge volta a perguntar num próximo arranque.",
            "Não foi possível concluir a instalação dos mods Complete.",
            "O conteúdo já instalado foi preservado. O PulseForge pode continuar e tentará novamente num próximo arranque.",
            "a preparar instalação", "a cancelar...", "mods", "ficheiros",
        };
    case SystemUiLanguage::spanish:
        return {
            "Sí", "No",
            "PulseForge Complete tiene 1 mod para instalar/actualizar (",
            "PulseForge Complete tiene {count} mods para instalar/actualizar (",
            " archivos).", "¿Instalar los mods ahora?",
            "Los archivos se transfieren directamente desde Google Drive a la carpeta de mods. Si eliges No, PulseForge volverá a preguntarlo en un próximo inicio.",
            "No se pudo completar la instalación de los mods Complete.",
            "El contenido ya instalado se ha conservado. PulseForge puede continuar y volverá a intentarlo en un próximo inicio.",
            "preparando instalación", "cancelando...", "mods", "archivos",
        };
    case SystemUiLanguage::french:
        return {
            "Oui", "Non",
            "PulseForge Complete a 1 mod à installer/mettre à jour (",
            "PulseForge Complete a {count} mods à installer/mettre à jour (",
            " fichiers).", "Installer les mods maintenant ?",
            "Les fichiers sont transférés directement depuis Google Drive vers le dossier des mods. Si vous choisissez Non, PulseForge vous le redemandera lors d'un prochain démarrage.",
            "L'installation des mods Complete n'a pas pu être terminée.",
            "Le contenu déjà installé a été conservé. PulseForge peut continuer et réessaiera lors d'un prochain démarrage.",
            "préparation de l'installation", "annulation...", "mods", "fichiers",
        };
    case SystemUiLanguage::german:
        return {
            "Ja", "Nein",
            "PulseForge Complete hat 1 Mod zum Installieren/Aktualisieren (",
            "PulseForge Complete hat {count} Mods zum Installieren/Aktualisieren (",
            " Dateien).", "Mods jetzt installieren?",
            "Die Dateien werden direkt von Google Drive in den Mods-Ordner übertragen. Bei Nein fragt PulseForge bei einem späteren Start erneut.",
            "Die Installation der Complete-Mods konnte nicht abgeschlossen werden.",
            "Bereits installierte Inhalte wurden beibehalten. PulseForge kann fortfahren und versucht es bei einem späteren Start erneut.",
            "Installation wird vorbereitet", "Abbruch...", "Mods", "Dateien",
        };
    case SystemUiLanguage::italian:
        return {
            "Sì", "No",
            "PulseForge Complete ha 1 mod da installare/aggiornare (",
            "PulseForge Complete ha {count} mod da installare/aggiornare (",
            " file).", "Installare i mod adesso?",
            "I file vengono trasferiti direttamente da Google Drive nella cartella dei mod. Se scegli No, PulseForge lo chiederà di nuovo a un avvio successivo.",
            "Impossibile completare l'installazione dei mod Complete.",
            "Il contenuto già installato è stato conservato. PulseForge può continuare e riproverà a un avvio successivo.",
            "preparazione installazione", "annullamento...", "mod", "file",
        };
    case SystemUiLanguage::dutch:
        return {
            "Ja", "Nee",
            "PulseForge Complete heeft 1 mod om te installeren/bij te werken (",
            "PulseForge Complete heeft {count} mods om te installeren/bij te werken (",
            " bestanden).", "Mods nu installeren?",
            "Bestanden worden rechtstreeks van Google Drive naar de mods-map overgebracht. Als je Nee kiest, vraagt PulseForge het bij een volgende start opnieuw.",
            "De Complete-mods konden niet volledig worden geïnstalleerd.",
            "Reeds geïnstalleerde inhoud is behouden. PulseForge kan doorgaan en probeert het bij een volgende start opnieuw.",
            "installatie voorbereiden", "annuleren...", "mods", "bestanden",
        };
    case SystemUiLanguage::polish:
        return {
            "Tak", "Nie",
            "PulseForge Complete ma 1 mod do zainstalowania/aktualizacji (",
            "PulseForge Complete ma {count} mody/modów do zainstalowania/aktualizacji (",
            " plików).", "Zainstalować mody teraz?",
            "Pliki są przesyłane bezpośrednio z Google Drive do folderu modów. Po wybraniu Nie PulseForge zapyta ponownie przy następnym uruchomieniu.",
            "Nie udało się dokończyć instalacji modów Complete.",
            "Już zainstalowana zawartość została zachowana. PulseForge może działać dalej i spróbuje ponownie przy następnym uruchomieniu.",
            "przygotowywanie instalacji", "anulowanie...", "mody", "pliki",
        };
    case SystemUiLanguage::russian:
        return {
            "Да", "Нет",
            "В PulseForge Complete есть 1 мод для установки/обновления (",
            "В PulseForge Complete есть {count} модов для установки/обновления (",
            " файлов).", "Установить моды сейчас?",
            "Файлы передаются напрямую из Google Drive в папку mods. Если выбрать Нет, PulseForge спросит снова при следующем запуске.",
            "Не удалось завершить установку модов Complete.",
            "Уже установленное содержимое сохранено. PulseForge может продолжить работу и повторит попытку при следующем запуске.",
            "подготовка установки", "отмена...", "моды", "файлы",
        };
    case SystemUiLanguage::japanese:
        return {
            "はい", "いいえ",
            "PulseForge Complete にはインストール/更新する Mod が 1 個あります (",
            "PulseForge Complete にはインストール/更新する Mod が {count} 個あります (",
            " ファイル)。", "今すぐ Mod をインストールしますか？",
            "ファイルは Google Drive から mods フォルダーへ直接転送されます。「いいえ」を選ぶと、次回起動時に再度確認します。",
            "Complete Mod のインストールを完了できませんでした。",
            "既にインストール済みの内容は保持されています。PulseForge は続行でき、次回起動時に再試行します。",
            "インストールを準備中", "キャンセル中...", "Mod", "ファイル",
        };
    case SystemUiLanguage::korean:
        return {
            "예", "아니요",
            "PulseForge Complete에 설치/업데이트할 모드가 1개 있습니다 (",
            "PulseForge Complete에 설치/업데이트할 모드가 {count}개 있습니다 (",
            "개 파일).", "지금 모드를 설치하시겠습니까?",
            "파일은 Google Drive에서 mods 폴더로 직접 전송됩니다. 아니요를 선택하면 다음 실행 시 다시 묻습니다.",
            "Complete 모드 설치를 완료할 수 없습니다.",
            "이미 설치된 콘텐츠는 보존되었습니다. PulseForge는 계속 실행할 수 있으며 다음 실행 시 다시 시도합니다.",
            "설치 준비 중", "취소 중...", "모드", "파일",
        };
    case SystemUiLanguage::chinese:
        return {
            "是", "否",
            "PulseForge Complete 有 1 个模组需要安装/更新 (",
            "PulseForge Complete 有 {count} 个模组需要安装/更新 (",
            " 个文件)。", "现在安装模组吗？",
            "文件会从 Google Drive 直接传输到 mods 文件夹。选择“否”后，PulseForge 会在下次启动时再次询问。",
            "无法完成 Complete 模组安装。",
            "已安装的内容已保留。PulseForge 可以继续运行，并会在下次启动时重试。",
            "正在准备安装", "正在取消...", "模组", "文件",
        };
    case SystemUiLanguage::english:
    default:
        return {
            "Yes", "No",
            "PulseForge Complete has 1 mod to install/update (",
            "PulseForge Complete has {count} mods to install/update (",
            " files).", "Install the mods now?",
            "Files are transferred directly from Google Drive into the mods folder. If you choose No, PulseForge will ask again on a later launch.",
            "PulseForge could not complete the Complete mod installation.",
            "Already installed content was preserved. PulseForge can continue and will try again on a later launch.",
            "preparing installation", "cancelling...", "mods", "files",
        };
    }
}

[[nodiscard]] inline std::string replace_count_token(
    const std::string_view source,
    const std::size_t count
) {
    constexpr std::string_view token{"{count}"};
    const auto position = source.find(token);
    if (position == std::string_view::npos) return std::string(source);
    std::string result;
    result.reserve(source.size() + 16U);
    result.append(source.substr(0U, position));
    result.append(std::to_string(count));
    result.append(source.substr(position + token.size()));
    return result;
}

}  // namespace pulseforge::detail
