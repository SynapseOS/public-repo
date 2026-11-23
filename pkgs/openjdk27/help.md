# OpenJDK 27

Пакет `openjdk27` содержит JDK Amazon Corretto 27.0.1.1+4 для x86_64 и musl.
После установки командой `setup install openjdk27` доступны `java27`, `javac27`, `jar27` и `jlink27`.
Остальные консольные инструменты и заголовки JNI находятся в `/usr/lib/jvm/java-27-openjdk`.
В этом же каталоге лежат сертификаты Corretto, сведения о сборке и лицензии модулей.

Пример файла `Hello.java`

```java
public class Hello {
    public static void main(String[] args) {
        System.out.println("Hello SynapseOS " + Runtime.version().feature());
    }
}
```

```sh
javac27 --release 27 Hello.java
java27 -cp . Hello
jar27 --create --file hello.jar --main-class Hello Hello.class
java27 -jar hello.jar
```

Создание отдельного runtime с модулем `java.base`

```sh
jlink27 --add-modules java.base --output runtime27
runtime27/bin/java -version
```

Для сборки образов в JDK включены JMOD. В модуле `java.base` поставляется zlib для автономного запуска.
Для уменьшения пакета исключены исходники, страницы руководства и образы CDS.
Для AWT и Swing пакет содержит библиотеки X11, шрифтов и ALSA. Окна работают в сеансе
XWayland. Для игр с LWJGL нужны Mesa/OpenGL и графические библиотеки самой игры.
Minecraft 1.20.4 использует системный GLFW из пакета `glfw` со свойством
`-Dorg.lwjgl.glfw.libname=/usr/lib/libglfw.so.3`.
Профилировщик async-profiler в пакет не включён.
Команда `java` и пакет `openjdk-current` сохраняют выбранную ранее версию Java.
