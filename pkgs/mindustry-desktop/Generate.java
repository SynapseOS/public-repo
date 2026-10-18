// Generate.java: генерация JNI из закреплённых исходников Arc
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: сборка графического клиента
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный
import com.badlogic.gdx.jnigen.NativeCodeGenerator;

public final class Generate {
	// Выполнить штатный генератор Arc
	public static void main(String[] arguments) throws Exception {
		new NativeCodeGenerator( ).generate(arguments[0], arguments[1], arguments[2]);
	}
}
