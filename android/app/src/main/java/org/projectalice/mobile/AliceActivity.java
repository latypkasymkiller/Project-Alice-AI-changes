package org.projectalice.mobile;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

/**
 * NativeActivity com o minimo que o C++ nao consegue fazer sozinho:
 * pedir acesso aos arquivos, abrir o seletor de pastas do Android e mostrar
 * mensagens. Todo o resto (janela, EGL, toque, jogo) continua na libAlice.so.
 *
 * Os metodos publicos sao chamados pelo C++ (entry_point_android.cpp), de
 * outra thread -- por isso tudo que mexe com a interface vai por runOnUiThread.
 */
public class AliceActivity extends NativeActivity {
	private static final int REQUEST_PICK_FOLDER = 1;

	// a NativeActivity carrega a .so por conta propria (dlopen), o que nao
	// registra os metodos "native" desta classe no JNI; carregar tambem pelo
	// System.loadLibrary resolve (e a mesma .so, nao carrega duas vezes)
	static {
		System.loadLibrary("Alice");
	}

	private static native void nativeOnGameFolderPicked(String path);

	private boolean waitingForAllFilesAccess = false;
	private AlertDialog folderDialog = null;

	private final Handler uiHandler = new Handler(Looper.getMainLooper());
	private AlertDialog loadingDialog = null;
	private PopupWindow statsPopup = null;
	private TextView statsText = null;
	private Runnable pendingLoadingDialog = null;
	private TextView loadingText = null;
	private String loadingMessage = null;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		hideSystemBars();
	}

	// O sistema mostra as barras de novo em varias situacoes (dialogos,
	// teclado, voltar de outro app); escondidas de novo ao recuperar o foco.
	@Override
	public void onWindowFocusChanged(boolean hasFocus) {
		super.onWindowFocusChanged(hasFocus);
		if(hasFocus)
			hideSystemBars();
	}

	/**
	 * Modo imersivo: esconde as barras de status e de navegacao (que em
	 * paisagem ficam na lateral, por cima do jogo). Deslizar a partir da borda
	 * as mostra por alguns segundos, sem tirar o jogo da tela cheia.
	 */
	private void hideSystemBars() {
		Window window = getWindow();
		window.setDecorFitsSystemWindows(false);
		WindowInsetsController controller = window.getInsetsController();
		if(controller != null) {
			controller.hide(WindowInsets.Type.systemBars());
			controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
		}
	}

	@Override
	protected void onResume() {
		super.onResume();
		if(waitingForAllFilesAccess) {
			waitingForAllFilesAccess = false;
			if(Environment.isExternalStorageManager()) {
				launchFolderPicker();
			} else {
				showFolderDialog("Sem a permissao \"Acesso a todos os arquivos\" o jogo nao consegue ler a pasta do Victoria 2.");
			}
		}
	}

	/** Chamado pelo C++ quando nao ha pasta do jogo valida configurada. */
	public void requestGameFolder(final String reason) {
		runOnUiThread(() -> showFolderDialog(reason));
	}

	/**
	 * Janela de carregamento, pedida pelo C++ (change_cursor "ocupado") antes
	 * de trabalhos que travam a thread do jogo -- ex.: criar a interface ao
	 * entrar na partida. Ela e desenhada pelo sistema, entao aparece e se anima
	 * mesmo com o jogo travado, e enquanto esta aberta os toques vao para ela:
	 * sem isso o Android mostrava "o app nao esta respondendo". So aparece se o
	 * trabalho passar de 300 ms, para carregamentos rapidos nao piscarem.
	 * Tambem mostra as etapas da abertura do app (extrair arquivos, montar e
	 * carregar o cenario): com ela aberta, uma mensagem nova so troca o texto.
	 * Mensagem vazia fecha.
	 */
	public void setLoading(final String message) {
		runOnUiThread(() -> {
			if(message == null || message.isEmpty()) {
				loadingMessage = null;
				if(pendingLoadingDialog != null) {
					uiHandler.removeCallbacks(pendingLoadingDialog);
					pendingLoadingDialog = null;
				}
				if(loadingDialog != null) {
					loadingDialog.dismiss();
					loadingDialog = null;
					loadingText = null;
				}
				return;
			}
			// aberta ou para abrir: so troca o texto (etapas do carregamento)
			loadingMessage = message;
			if(loadingDialog != null) {
				loadingText.setText(message);
				return;
			}
			if(pendingLoadingDialog != null)
				return;
			pendingLoadingDialog = () -> {
				pendingLoadingDialog = null;
				showLoadingDialog(loadingMessage);
			};
			uiHandler.postDelayed(pendingLoadingDialog, 300);
		});
	}

	private void showLoadingDialog(String message) {
		if(isFinishing() || message == null)
			return;
		int padding = (int)(24 * getResources().getDisplayMetrics().density);
		LinearLayout layout = new LinearLayout(this);
		layout.setOrientation(LinearLayout.HORIZONTAL);
		layout.setGravity(Gravity.CENTER_VERTICAL);
		layout.setPadding(padding, padding, padding, padding);
		ProgressBar spinner = new ProgressBar(this);
		spinner.setIndeterminate(true);
		layout.addView(spinner);
		loadingText = new TextView(this);
		loadingText.setText(message);
		loadingText.setTextSize(18);
		loadingText.setPadding(padding, 0, 0, 0);
		layout.addView(loadingText);
		loadingDialog = new AlertDialog.Builder(this)
			.setView(layout)
			.setCancelable(false)
			.create();
		loadingDialog.setCanceledOnTouchOutside(false);
		// Uma janela de dialogo nova traz as barras do sistema de volta. Aberta
		// sem foco, ja com as barras escondidas, e so depois recebe o foco.
		Window window = loadingDialog.getWindow();
		if(window != null)
			window.setFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
		loadingDialog.show();
		if(window != null) {
			WindowInsetsController controller = window.getInsetsController();
			if(controller != null) {
				controller.hide(WindowInsets.Type.systemBars());
				controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
			}
			window.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
		}
	}

	/**
	 * Contador de desempenho (ligado com toque de tres dedos), atualizado pela
	 * thread nativa uma vez por segundo. Fica numa janela propria por cima do
	 * jogo, porque a NativeActivity desenha direto na superficie e nao mostra
	 * views comuns. Nao recebe toques. Texto vazio fecha.
	 */
	public void setStats(final String text) {
		runOnUiThread(() -> {
			if(text == null || text.isEmpty()) {
				if(statsPopup != null) {
					statsPopup.dismiss();
					statsPopup = null;
					statsText = null;
				}
				return;
			}
			if(statsPopup == null) {
				if(isFinishing())
					return;
				int padding = (int)(4 * getResources().getDisplayMetrics().density);
				statsText = new TextView(this);
				statsText.setTextColor(0xFFFFFFFF);
				statsText.setBackgroundColor(0xA0000000);
				statsText.setTypeface(Typeface.MONOSPACE);
				statsText.setTextSize(12);
				statsText.setPadding(padding, padding, padding, padding);
				statsPopup = new PopupWindow(statsText, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, false);
				statsPopup.setTouchable(false);
				statsText.setText(text);
				statsPopup.showAtLocation(getWindow().getDecorView(), Gravity.TOP | Gravity.END, padding, padding);
				return;
			}
			statsText.setText(text);
		});
	}

	/** Mensagem curta na tela (ex.: "Criando o cenario..."). */
	public void showMessage(final String message) {
		runOnUiThread(() -> Toast.makeText(this, message, Toast.LENGTH_LONG).show());
	}

	private void showFolderDialog(String reason) {
		if(folderDialog != null && folderDialog.isShowing())
			folderDialog.dismiss();
		String message = "O Project Alice precisa dos arquivos do Victoria 2 (com as expansoes).\n\n"
			+ "Copie a pasta do jogo para o aparelho e escolha-a na proxima tela "
			+ "(a pasta que contem \"common\", \"map\" e \"gfx\").";
		if(reason != null && !reason.isEmpty())
			message = reason + "\n\n" + message;
		folderDialog = new AlertDialog.Builder(this)
			.setTitle("Pasta do Victoria 2")
			.setMessage(message)
			.setCancelable(false)
			.setPositiveButton("Escolher pasta", (d, w) -> ensureAccessThenPick())
			.setNegativeButton("Sair", (d, w) -> finish())
			.show();
	}

	private void ensureAccessThenPick() {
		// minSdk 31: o acesso direto (caminhos de arquivo, que o C++ usa) a
		// pastas fora do app exige "Acesso a todos os arquivos"
		if(Environment.isExternalStorageManager()) {
			launchFolderPicker();
			return;
		}
		waitingForAllFilesAccess = true;
		try {
			startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
				Uri.parse("package:" + getPackageName())));
		} catch(Exception e) {
			startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
		}
	}

	private void launchFolderPicker() {
		Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
		try {
			startActivityForResult(intent, REQUEST_PICK_FOLDER);
		} catch(Exception e) {
			showFolderDialog("Nao foi possivel abrir o seletor de pastas deste aparelho.");
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		super.onActivityResult(requestCode, resultCode, data);
		if(requestCode != REQUEST_PICK_FOLDER)
			return;
		if(resultCode != RESULT_OK || data == null || data.getData() == null) {
			showFolderDialog("Nenhuma pasta foi escolhida.");
			return;
		}
		String path = treeUriToPath(data.getData());
		if(path == null) {
			showFolderDialog("Escolha uma pasta do armazenamento do aparelho ou do cartao SD (nao de um servico na nuvem).");
			return;
		}
		nativeOnGameFolderPicked(path);
	}

	/**
	 * content://com.android.externalstorage.documents/tree/primary%3AGames%2FVictoria%202
	 *   -> /storage/emulated/0/Games/Victoria 2
	 * Cartao SD ("XXXX-XXXX:pasta") -> /storage/XXXX-XXXX/pasta
	 */
	static String treeUriToPath(Uri uri) {
		if(!"com.android.externalstorage.documents".equals(uri.getAuthority()))
			return null;
		String docId;
		try {
			docId = DocumentsContract.getTreeDocumentId(uri);
		} catch(Exception e) {
			return null;
		}
		int colon = docId.indexOf(':');
		if(colon < 0)
			return null;
		String volume = docId.substring(0, colon);
		String relative = docId.substring(colon + 1);
		String base = "primary".equalsIgnoreCase(volume)
			? Environment.getExternalStorageDirectory().getAbsolutePath()
			: "/storage/" + volume;
		return relative.isEmpty() ? base : base + "/" + relative;
	}
}
