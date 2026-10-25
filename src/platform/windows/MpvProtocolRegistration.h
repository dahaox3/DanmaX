#pragma once
#include <QString>
class MpvProtocolRegistration {
  public:
    MpvProtocolRegistration(QString data, QString executable, QString classesRoot = "Software\\Classes");
    QString bind();
    QString restore();
    bool registered() const;
    QString command() const;
  private:
    QString data_, executable_, root_;
};
