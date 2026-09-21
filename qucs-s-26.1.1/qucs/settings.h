#include <map>
#include <QDebug>
#include <QSettings>
#include <qucs.h>

/*!
 * \brief The application's settings store, "qucs"/"qucs_s", opened the way
 *        every part of the application should open it: in
 *        QSettings::defaultFormat(), the platform's native store unless the
 *        process chose another - the tests do, to keep their runs out of
 *        the user's own preferences.
 */
class QucsSettingsFile : public QSettings
{
public:
  explicit QucsSettingsFile(QObject* parent = nullptr)
    : QSettings(QSettings::defaultFormat(), QSettings::UserScope,
                QStringLiteral("qucs"), QStringLiteral("qucs_s"), parent) {}
};

class settingsManager : public QucsSettingsFile
{ 
    // Q_OBJECT

// TODO: Make constructors private / protected so this class can only be
// instantiated by QucsSingleton.
public:
  explicit settingsManager();
  ~settingsManager();

  /** \brief Set all settings in the given group to their default value.
  */  
  void resetDefaults(const QString& group = "All");

  /** \brief Get the default value for a given setting.
  */  
  template<class T>
  T itemDefault(const QString& key)
  {
    return value(key).value<T>();
  }

  /** \brief Store a setting.
  */  
  template<class T>
  void setItem(const QString& key, const T& value)
  {
    setValue(key, value);
  }

  /** \brief Retrieve a setting value by its key.
  */  
  template<class T>
  T item(const QString& key)
  {
    if (contains(key)) {
      return value(key).value<T>();
    }

    for (auto alias : m_Aliases[key]) {
      if (contains(alias)) {
        // qDebug() << "Found alias: " << alias;
        return value(alias).value<T>();
      }
    }
    return m_Defaults[key].value<T>();
  }

private:
  std::map<QString, QVariant> m_Defaults;
  std::map<QString, QStringList> m_Aliases;

  /** \brief Populate the list of items that have a (hard coded) default value.
  */
  void initDefaults();

  /** \brief Populate the list of aliases.
   *         (legacy names for settings that have been used in the past).
  */
  void initAliases();  
};

typedef QucsSingleton<settingsManager> _settings;
